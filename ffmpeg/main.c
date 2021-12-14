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
	int force_refresh;
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
	double audio_clock;
	int audio_clock_serial;
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

static void update_video_pts(VideoState* is, double pts, int64_t pos, int serial)
{
	set_clock(&is->vidclk, pts, serial);
	sync_clock_to_slave(&is->extclk, &is->vidclk);
}

static void video_refresh(void* opaque, double* remaining_time)
{
	VideoState* is = opaque;
	double time;
	Frame* sp, * sp2;
	if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
		check_external_clock_speed(is);
	if (!display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st)
	{
		time = av_gettime_relative() / 1000000.0;
		if (is->force_refresh || is->last_vis_time + rdftspeed < time)
		{
			video_display(is);
			is->last_vis_time = time;
		}
		*remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - time);
	}
	if (is->video_st)
	{
	retry:
		if (frame_queue_nb_remaining(&is->pictq) == 0)
		{

		}
		else
		{
			double last_duration, duration, delay;
			Frame* vp, * lastvp;
			lastvp = frame_queue_peek_last(&is->pictq);
			vp = frame_queue_peek(&is->pictq);
			if (vp->serial != is->videoq.serial)
			{
				frame_queue_next(&is->pictq);
				goto retry;
			}
			if (lastvp->serial != vp->serial)
				is->frame_timer = av_gettime_relative() / 1000000.0;
			if (is->paused)
				goto display;

			last_duration = vp_duration(is, lastvp, vp);
			delay = compute_target_delay(last_duration, is);
			time = av_getting_relative() / 1000000.0;
			if (time < is.frame_timer + delay)
			{
				*remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
				goto display;
			}

			is->frame_timer += delay;
			if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
				is->frame_timer = time;
			SDL_LockMutex(is->pictq.mutex);
			if (!isnan(vp->pts))
				update_video_pts(is, vp->pts, vp->pos, vp->serial);
			SDL_UnlockMutex(is->pictq.mutex);

			if (frame_queue_nb_remaining(&is->pictq) > 1)
			{
				Frame* nextvp = frame_queue_peek_next(&is->pictq);
				duration = vp_duration(is, vp, nextvp);
				if (!is->step && (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration)
				{
					is->frame_drops_late++;
					frame_queue_next(&is->pictq);
					goto retry;
				}
			}
			if (is->subtitle_st)
			{
				while (frame_queue_nb_remaining(&is->subpq) > 0)
				{
					sp = frame_queue_peek(&is->subpq);
					if (frame_queue_nb_remaining(&is->subpq) > 1)
						sp2 = frame_queue_peek_next(&is->subpq);
					else
						sp2 = NULL;
					if (sp->serial != is->subtitleq.serial || (is->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000)))
						|| (sp2 && is->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))
					{
						if (sp->uploaded)
						{
							int i;
							for (i = 0; i < sp->sub.num_rects; i++)
							{
								AVSubtitleRect* sub_rect = sp->sub.rects[i];
								uint8_t* pixels;
								int pitch, j;
								if (!SDL_LockTexture(is->sub_texture, (SDL_Rect*)sub_rect, (void**)&pixels, &pitch))
								{
									for (j = 0; j < sub_rect->h; j++, pixels += pitch)
										memset(pixels, 0, sub_rect->w << 2);
									SDL_UnlockTexture(is->sub_texture);
								}
							}
						}
						frame_queue_next(&is->subpq);
					}
					else
						break;
				}
			}
		frame_queue_next(&is->pictq);
		is->force_refresh = 1;
		if (is->step && !is->paused)
			stream_toggle_pause(is);
		}
	display:
		if (!display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
			video_display(is);
	}
	is->force_refresh = 0;
	if (show_status)
	{
		AVBPrint buf;
		static int64_t last_time;
		int64_t cur_time;
		int aqsize, vqsize, sqsize;
		double av_diff;
		cur_time = av_gettime_relative();
		if (!last_time || (cur_time - last_time) >= 30000)
		{
			aqsize = 0;
			vqsize = 0;
			sqsize = 0;
			if (is->audio_st)
				aqsize = is->audioq.size;
			if (is->video_st)
				vqsize = is->videoq.size;
			if (is->subtitle_st)
				sqsize = is->subtitleq.size;
			av_diff = 0;
			if (is->audio_st && is->video_st)
				av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
			else if (is->video_st)
				av_diff = get_master_clock(is) - get_clock(&is->vidclk);
			else if (is->audio_st)
				av_diff = get_master_clock(is) - get_clock(&is->audclk);

			av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
			av_bprint(&buf,
				"%7.2f %s:%7.3f fd=%4d aq=%5dKB vp=%5dKB sq=%5dB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
				get_master_clock(is),
				(is->audio_st&& is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
				av_diff,
				is->frame_drops_early + is->frame_drops_late,
				sqsize / 1024,
				vqsize / 1024,
				sqsize,
				is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
				is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);

			if (show_status == 1 && AV_LOG_INFO > av_log_get_level())
				fprintf(stderr, "%s", buf.str);
			else
				av_log(NULL, AV_LOG_INFO, "%s", buf.str);
			fflush(stderr);
			av_bprint_finalize(&buf, NULL);
			last_time = cur_time;
		}
	}
}

static int queue_picture(VideoState* is, AVFrame* src_frame, double pts, double duration, int64_t pos, int serial)
{
	Frame* vp;
#if defined(DEBUG_SYNC)
	printf("frame_type=%c pts=%0.3f\n", av_get_picture_type_char(src_frame->pict_type), pts);
#endif
	if(!(vp = frame_queue_peek_writable(&is->pictq)))
		return -1;

	vp->sar = src_frame->sample_aspect_ratio;
	vp->uploaded = 0;

	vp->width = src_frame->width;
	vp->height = src_frame->height;
	vp->format = src_frame->format;
	vp->pts = pts;
	vp->duration = duration;
	vp->pos = pos;
	vp->serial = serial;

	set_default_window_size(vp->width, vp->height, vp->sar);
	av_frame_move_ref(vp->frame, src_frame);
	frame_queue_push(&is->pictq);
	return 0;
}

static int get_video_frame(VideoState* is, AVFrame* frame)
{
	int got_picture;
	if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
		return -1;
	if (got_picture)
	{
		double dpts = NAN;
		if (frame->pts != AV_NOPTS_VALUE)
			dpts = av_q2d(is->video_st->time_base) * frame->pts;
		frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);
		if (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))
		{
			if (frame->pts != AV_NOPTS_VALUE)
			{
				double diff = dpts - get_master_clock(is);
				if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
					diff - is->frame_last_filter_delay < 0 &&
					is->viddec.pkt_serial == is->vidclk.serial &&
					is->videoq.nb_packets)
				{
					is->frame_drops_early++;
					av_frame_unref(frame);
					got_picture = 0;
				}
			}
		}
	}
	return got_picture;
}

static int audio_thread(void* arg)
{
	VideoState* is = arg;
	AVFrame* frame = av_frame_alloc();
	Frame* af;
	int got_frame = 0;
	AVRational tb;
	int ret = 0;
	if (!frame)
		return AVERROR(ENOMEM);
	do 
	{
		if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0)
			goto the_end;
		if (got_frame)
		{
			tb = (AVRational){ 1, frame->sample_rate };
			if (!(af = frame_queue_peek_writable(&is->sampq)))
				goto the_end;
			af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
			af->pos = frame->pkt_pos;
			af->serial = is->auddec.pkt_serial;
			af->duration = av_q2d((AVRational) { frame->nb_samples, frame->sample_rate });
			av_frame_move_ref(af->frame, frame);
			frame_queu_push(&is->sampq);
		}
	} while (ret>=0||ret==AVERROR(EAGAIN)||ret == AVERROR_EOF);
the_end:
	av_frame_free(&frame);
	return ret;
}

static int decoder_start(Decoder* d, int (*fn)(void*), const char* thread_name, void* arg)
{
	packet_queue_start(d->queue);
	d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
	if (!d->decoder_tid)
	{
		av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread():%s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	return 0;
}

static int video_thread(void* arg)
{
	VideoState* is = arg;
	AVFrame* frame = av_frame_alloc();
	double pts;
	double duration;
	int ret;
	AVRational tb = is->video_st->time_base;
	AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

	if (!frame)
		return AVERROR(ENOMEM);
	for (;;)
	{
		ret = get_video_frame(is, frame);
		if (ret < 0)
			goto the_end;
		if(!ret)
			continue;
		duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational) { frame_rate.den, frame_rate.num }) : 0);
		pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
		ret = queue_picture(is, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
		av_frame_unref(frame);
		if (ret < 0)
			goto the_end;
	}
the_end:
	av_frame_free(&frame);
	return 0;
}

static int subtitle_thread(void* arg)
{
	VideoState* is = arg;
	Frame* sp;
	int got_subtitle;
	double pts;
	for (;;)
	{
		if (!(sp = frame_queue_peek_writable(&is->subpq)))
			return 0;
		if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0)
			break;
		pts = 0;

		if (got_subtitle && sp->sub.format == 0)
		{
			if (sp->sub.pts != AV_NOPTS_VALUE)
				pts = sp->sub.pts / (double)AV_TIME_BASE;
			sp->pts = pts;
			sp->serial = is->subdec.pkt_serial;
			sp->width = is->subdec.avctx->width;
			sp->height = is->subdec.avctx->height;
			sp->uploaded = 0;
			frame_queue_push(&is->subpq);
		}
		else if (got_subtitle)
			avsubtitle_free(&sp->sub);
	}
	return 0;
}

static void update_sample_display(VideoState* is, short* samples, int samples_size)
{
	int size, len;
	size = samples_size / sizeof(short);
	while (size > 0)
	{
		len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
		if (len > size)
			len = size;
		memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
		samples += len;
		is->sample_array_index += len;
		if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
			is->sample_array_index = 0;
		size -= len;
	}
}

static int synchronize_audio(VideoState* is, int nb_samples)
{
	int wanted_nb_samples = nb_samples;
	if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTYER)
	{
		double diff, avg_diff;
		int min_nb_samples, max_nb_samples;
		diff = get_clock(&is->audclk) - get_master_clock(is);
		if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD)
		{
			is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
			if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB)
				is->audio_diff_avg_count++;
			else
			{
				avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
				if (fabs(avg_diff) >= is->audio_diff_threshold)
				{
					wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
					min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
					max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
					wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
				}
				av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
					diff, avg_diff, wanted_nb_samples - nb_samples,
					is->audio_clock, is->audio_diff_threshold);
			}
		}
		else
		{
			is->audio_diff_avg_count = 0;
			is->audio_diff_cum = 0;
		}
	}
	return wanted_nb_samples;
}

static int audio_decode_frame(VideoState* is)
{
	int data_size, resampled_data_size;
	int64_t dec_channel_layout;
	av_unused double audio_clock0;
	int wanted_nb_samples;
	Frame* af;
	if (is->paused)
		return -1;
	do 
	{
#if defined(_WIN32)
		while (frame_queue_nb_remaining(&is->sampq) == 0)
		{
			if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
				return -1;
			av_usleep(1000);
		}
#endif
		if (!(af = frame_queue_peek_readable(&is->sampq)))
			return -1;
		frame_queue_next(&is->sampq);
	} while (af->serial!=is->audioq.serial);

	data_size = av_samples_get_buffer_size(NULL, af->frame->channels,
		af->frame->nb_samples, af->frame->format, 1);
	dec_channel_layout = (af->frame->channel_layout && af->frame->channels == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
		af->frame->channel_layout : av_get_default_channel_layout(af->frame->channels);
	wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);
	if (af->frame->format != is->audio_src.fmt ||
		dec_channel_layout != is->audio_src.channel_layout ||
		af->frame->sample_rate != is->audio_src.freq ||
		(wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx))
	{
		swr_free(&is->swr_ctx);
		is->swr_ctx = swr_alloc_set_opts(NULL,
			is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
			dec_channel_layout, af->frame->format, af->frame->sample_rate,
			0, NULL);
		if (!is->swr_ctx || swr_init(is->swr_ctx) < 0)
		{
			av_log(NULL, AV_LOG_ERROR,
				"Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
				af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->channels,
				is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
			swr_free(&is->swr_ctx);
			return -1;
		}
		is->audio_src.channel_layout = dec_channel_layout;
		is->audio_src.channels = af->frame->channels;
		is->audio_src.freq = af->frame->sample_rate;
		is->audio_src.fmt = af->frame->format;
	}
	if (is->swr_ctx)
	{
		const uint8_t** in = (const uint8_t**)af->frame->extended_data;
		uint8_t** out = &is->audio_buf1;
		int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
		int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
		int len2;
		if (out_size < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
			return -1;
		}
		if (wanted_nb_samples != af->frame->nb_samples)
		{
			if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
				wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0)
			{
				av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
				return -1;
			}
		}
		av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
		if (!is->audio_buf1)
			return AVERROR(ENOMEM);
		len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
		if (len2 < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
			return -1;
		}
		if (len2 == out_count)
		{
			av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
			if (swr_init(is->swr_ctx) < 0)
				swr_free(&is->swr_ctx);
		}
		is->audio_buf = is->audio_buf1;
		resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
	}
	else
	{
		is->audio_buf = af->frame->data[0];
		resampled_data_size = data_size;
	}

	audio_clock0 = is->audio_clock;
	if (!isnan(af->pts))
		is->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
	else
		is->audio_clock = NAN;
	is->audio_clock_serial = af->serial;
#ifdef DEBUG
	{
		static double last_clock;
		printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
			is->audio_clock - last_clock,
			is->audio_clock, audio_clock0);
		last_clock = is->audio_clock;
	}
#endif
	return resampled_data_size;
}

static void sdl_audio_callback(void* opaque, Uint8* stream, int len)
{
	VideoState* is = opaque;
	int audio_size, len1;
	audio_callback_time = av_gettime_relative();
	while (len > 0)
	{
		if (is->audio_buf_index >= is->audio_buf_size)
		{
			audio_size = audio_decode_frame(is);
			if (audio_size < 0)
			{
				is->audio_buf = NULL;
				is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
			}
			else
			{
				if (is->show_mode != SHOW_MODE_VIDEO)
					update_sample_display(is, (int16_t*)is->audio_buf, audio_size);
				is->audio_buf_size = audio_size;
			}
			is->audio_buf_index = 0;
		}
		len1 = is->audio_buf_size - is->audio_buf_index;
		if (len1 > len)
			len1 = len;
		if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
		{
			memcpy(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, len1);
		}
		else
		{
			memset(stream, 0, len1);
			if (!is->muted && is->audio_buf)
				SDL_MixAudioFormat(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1, is->audio_volume);
		}
		len -= len1;
		stream += len1;
		is->audio_buf_index += len1;
	}
	is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
	if (!isnan(is->audio_clock))
	{
		set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, audio_callback_time / 1000000.0);
		sync_clock_to_slave(&is->extclk, &is->audclk);
	}
}

static int audio_open(void* opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams* audio_hw_params)
{
	SDL_AudioSpec wanted_spec, spec;
	const char* env;
	static const int next_nb_channels[] = { 0,0,1,6,2,6,4,6 };
	static const int next_sample_rates[] = { 0,44100, 48000, 96000, 192000 };
	int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
	env = SDL_getenv("SDL_AUDIO_CHANNELS");
	if (env)
	{
		wanted_nb_channels = atoi(env);
		wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
	}
	if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout))
	{
		wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
		wanted_channel_layout &= -AV_CH_LAYOUT_STEREO_DOWNMIX;
	}
	wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
	wanted_spec.channels = wanted_nb_channels;
	wanted_spec.freq = wanted_sample_rate;
	if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
		return -1;
	}
	while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
		next_sample_rate_idx--;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.silence = 0;
	wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
	wanted_spec.callback = sdl_audio_callback;
	wanted_spec.userdata = opaque;
	while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE)))
	{
		av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
			wanted_spec.channels, wanted_spec.freq, SDL_GetError());
		wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
		if (!wanted_spec.channels)
		{
			wanted_spec.freq = next_nb_channels[FFMIN(7, wanted_spec.channels)];
			if (!wanted_spec.channels)
			{
				wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
				wanted_spec.channels = wanted_nb_channels;
				if (!wanted_spec.freq)
				{
					av_log(NULL, AV_LOG_ERROR, "No more combination to try, audio open failed\n");
					return -1;
				}
			}
			wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
		}
		if (spec.format != AUDIO_S16SYS)
		{
			av_log(NULL, AV_LOG_ERROR, "SDL advised audio format %d is not supported!\n", spec.channels);
			return -1;
		}
	}
	audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
	audio_hw_params->freq = spec.freq;
	audio_hw_params->channel_layout = wanted_channel_layout;
	audio_hw_params->channels = spec.channels;
	audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
	audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
	if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0)
	{
		av_log(NULL, AV_LOG_ERROR, "av_sample_get_buffer_size failed\n");
		return -1;
	}
	return spec.size;
}

static int stream_component_open(VideoState* is, int stream_index)
{
	AVFormatContext* ic = is->ic;
	AVCodecContext* avctx;
	const AVCodec* codec;
	const char* forced_codec_name = NULL;
	AVDictionary* opts = NULL;
	AVDictionaryEntry* t = NULL;
	int sample_rate, nb_channels;
	int64_t channel_layout;
	int ret = 0;
	int stream_lowres = lowres;
	if (stream_index < 0 || stream_index >= ic->nb_streams)
		return -1;
	avctx = avcodec_alloc_context3(NULL);
	if (!avctx)
		return AVERROR(ENOMEM);
	ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
	if (ret < 0)
		goto fail;
	avctx->pkt_timebase = ic->streams[stream_index]->time_base;
	codec = avcodec_find_decoder(avctx->codec_id);
	switch (avctx->codec_type)
	{
	case AVMEDIA_TYPE_AUDIO:
		is->last_audio_stream = stream_index;
		forced_codec_name = audio_codec_name;
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		is->last_subtitle_stream = stream_index;
		forced_codec_name = subtitle_codec_name;
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->last_video_stream = stream_index;
		forced_codec_name = video_codec_name;
		break;
	}

	avctx->codec_id = codec->id;
	if (stream_lowres > codec->max_lowres)
	{
		av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n", codec->max_lowres);
		stream_lowres = codec->max_lowres;
	}
	avctx->lowres = stream_lowres;
	if (fast)
		avctx->flags2 |= AV_CODEC_FLAG2_FAST;
	opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
	if (!av_dict_get(opts, "threads", NULL, 0))
		av_dict_set(&opts, "threads", "auto", 0);
	if (stream_lowres)
		av_dict_set_int(&opts, "lowres", stream_lowres, 0);
	if ((ret = avcodec_open2(avctx, codec, &opts)) < 0)
		goto fail;
	if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX)))
	{
		av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
		ret = AVERROR_OPTION_NOT_FOUND;
		goto fail;
	}
	is->eof = 0;
	ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
	switch (avctx->codec_type)
	{
	case AVMEDIA_TYPE_AUDIO:
		sample_rate = avctx->sample_rate;
		nb_channels = avctx->channels;
		channel_layout = avctx->channel_layout;
		if ((ret = audio_open(is, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
			goto fail;
		is->audio_hw_buf_size = ret;
		is->audio_src = is->audio_tgt;
		is->audio_buf_size = 0;
		is->audio_buf_index = 0;

		is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
		is->audio_diff_avg_count = 0;
		is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;
		is->audio_stream = stream_index;
		is->audio_st = ic->streams[stream_index];
		if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread)) < 0)
			goto fail;
		if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek)
		{
			is->auddec.start_pts = is->audio_st->start_time;
			is->auddec.start_pts_tb = is->audio_st->time_base;
		}
		if ((ret = decoder_start(&is->auddec, audio_thread, "audio_decoder", is)) < 0)
			goto out;
		SDL_PauseAudioDevice(audio_dev, 0);
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->video_stream = stream_index;
		is->video_st = ic->streams[stream_index];
		if ((ret = decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread)) < 0)
			goto fail;
		if ((ret = decoder_start(&is->viddec, video_thread, "video_decoder", is)) < 0)
			goto out;
		is->queue_attachments_req = 1;
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		is->subtitle_stream = stream_index;
		is->subtitle_st = ic->streams[stream_index];
		if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread)) < 0)
			goto fail;
		if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", is)) < 0)
			goto out;
		break;

	default:
		break;
	}
	goto out;
fail:
	avcodec_free_context(&avctx);
out:
	av_dict_free(&opts);
	return ret;
}

static int decode_interrupt_cb(void* ctx)
{
	VideoState* is = ctx;
	return is->abort_request;
}

static int stream_has_enough_packets(AVStream* st, int stream_id, PacketQueue* queue)
{
	return stream_id<0 || queue->abort_request || (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
		queue->nb_packets>MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

static int is_realtime(AVFormatContext* s)
{
	if (!strcmp(s->iformat->name, "rtp")
		|| !strcmp(s->iformat->name, "rtsp")
		|| !strcmp(s->iformat->name, "sdp"))
		return 1;
	if(s->pb&&(!strncmp(s->url, "rtp:",4)||!strncmp(s->url, "udp:",4)))
		return 1;
	return 0;
}

static int read_thread(void* arg)
{
	VideoState* is = arg;
	AVFormatContext* ic = NULL;
	int err, i, ret;
	int st_index[AVMEDIA_TYPE_NB];
	AVPacket* pkt = NULL;
	int64_t stream_start_time;
	int pkt_in_play_range = 0;
	AVDictionaryEntry* t;
	SDL_mutex* wait_mutex = SDL_CreateMutex();
	int scan_all_pmts_set = 0;
	int64_t pkt_ts;

	if (!wait_mutex)
	{
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		ret = AVERROR(ENOMEM);
		goto fail;
	}

	memset(st_index, -1, sizeof(st_index));
	is->eof = 0;
	pkt = av_packet_alloc();
	if (!pkt)
	{
		av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
		ret = AVERROR(ENOMEM);
		goto fail;
	}
	ic = avformat_alloc_context();
	if (!ic)
	{
		av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
		ret = AVERROR(ENOMEM);
		goto fail;
	}
	ic->interrupt_callback.callback = decode_interrupt_cb;
	ic->interrupt_callback.opaque = is;
	if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE))
	{
		av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
		scan_all_pmts_set = 1;
	}
	err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
	if (err < 0)
	{
		print_error(is->filename, err);
		ret = -1;
		goto fail;
	}
	if (scan_all_pmts_set)
		av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
	if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX)))
	{
		av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
		ret = AVERROR_OPTION_NOT_FOUND;
		goto fail;
	}
	is->ic = ic;
	if (genpts)
		ic->flags |= AVFMT_FLAG_GENPTS;
	av_format_inject_global_side_data(ic);

	if (find_stream_info)
	{
		AVDictionary** opts = setup_find_stream_info_opts(ic, codec_opts);
		int orig_nb_streams = ic->nb_streams;
		err = avformat_find_stream_info(ic, opts);
		for (i = 0; i < orig_nb_streams; i++)
			av_dict_free(&opts[i]);
		av_freep(&opts);
		if (err < 0)
		{
			av_log(NULL, AV_LOG_WARNING, "%s: could not find codec parameters\n", is->filename);
			ret = -1;
			goto fail;
		}
	}
	if (ic->pb)
		ic->pb->eof_reached = 0;
	if (seek_by_bytes < 0)
		seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);
	is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
	if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
		window_title = av_asprintf("%s - %s", t->value, input_filename);
	if (start_time != AV_NOPTS_VALUE)
	{
		int64_t timestamp;
		if (ic->start_time != AV_NOPTS_VALUE)
			timestamp += ic->start_time;
		ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n", is->filename, (double)timestamp / AV_TIME_BASE);
		}
	}

	is->realtime = is_realtime(ic);
	if (show_status)
		av_dump_format(ic, 0, is->filename, 0);
	for (i = 0; i < ic->nb_streams; i++)
	{
		AVStream* st = ic->streams[i];
		enum AVMediaType type = st->codecpar->codec_type;
		st->discard = AVDISCARD_ALL;
		if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
			if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
				st_index[type] = i;
	}
	for (i = 0; i < AVMEDIA_TYPE_NB; i++)
	{
		if (wanted_stream_spec[i] && st_index[i] == -1)
		{
			av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", wanted_stream_spec[i], av_get_media_type_string(i));
			st_index[i] = INT_MAX;
		}
	}

	if (!video_disable)
		st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
	if (!audio_disable)
		st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, st_index[AVMEDIA_TYPE_AUDIO], st_index[AVMEDIA_TYPE_VIDEO], NULL, 0);
	if (!video_disable && !subtitle_disable)
		st_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE, st_index[AVMEDIA_TYPE_SUBTITLE],
			(st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO] : st_index[AVMEDIA_TYPE_VIDEO]), NULL, 0);
	is->show_mode = show_mode;
}




























//#include "MySDL.h"
//extern "C"
//{
//#include "libavutil/avstring.h"
//#include "libavutil/channel_layout.h"
//#include "libavutil/eval.h"
//#include "libavutil/mathematics.h"
//#include "libavutil/pixdesc.h"
//#include "libavutil/imgutils.h"
//#include "libavutil/dict.h"
//#include "libavutil/fifo.h"
//#include "libavutil/parseutils.h"
//#include "libavutil/samplefmt.h"
//#include "libavutil/time.h"
//#include "libavutil/bprint.h"
//#include "libavformat/avformat.h"
//#include "libswscale/swscale.h"
//#include "libavutil/opt.h"
//#include "libavcodec/avcodec.h"
//#include "libavcodec/avfft.h"
//#include "libswresample/swresample.h"
//}
//
//
//const char program_name[] = "FFplay_dss";
//int MySDL::Init(int audio_disable, int display_disable, int alwaysontop, int borderless, SDL_RendererInfo renderer_info)
//{
//	int flags;
//
//	flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
//	if (audio_disable)
//		flags &= -SDL_INIT_AUDIO;
//	else
//		if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BEFFER_SIZE"))
//			SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
//
//	if (display_disable)
//		flags &= -SDL_INIT_VIDEO;
//	if (SDL_Init(flags))
//	{
//		av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
//		av_log(NULL, AV_LOG_FATAL, "(Did you set DISPLAY variable?)\n");
//		exit(1);
//	}
//
//	SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
//	SDL_EventState(SDL_USEREVENT, SDL_IGNORE);
//	if (!display_disable)
//	{
//		int flags = SDL_WINDOW_HIDDEN;
//		if (alwaysontop)
//			flags |= SDL_WINDOW_ALWAYS_ON_TOP;
//		if (borderless)
//			flags |= SDL_WINDOW_BORDERLESS;
//		else
//			flags |= SDL_WINDOW_RESIZABLE;
//		m_window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width, default_height, flags);
//		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
//		if (m_window)
//		{
//			m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
//			if (!m_renderer)
//			{
//				av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
//				m_renderer = SDL_CreateRenderer(m_window, -1, 0);
//			}
//			if (m_renderer)
//			{
//				if (!SDL_GetRendererInfo(m_renderer, &renderer_info))
//					av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
//			}
//		}
//		if (!m_window || !m_renderer || !renderer_info.num_texture_formats)
//		{
//			av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
//			
//			return -1;
//		}
//	}
//	return 0;
//}
//

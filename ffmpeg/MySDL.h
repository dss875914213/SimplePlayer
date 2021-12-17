#include "SDL/SDL.h"
#include "SDL/SDL_thread.h"
#include "libavformat/avformat.h"
#include "libavfilter/avfilter.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
//#include "libpostproc/postprocess.h"
#include "libavutil/attributes.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/display.h"
#include "libavutil/mathematics.h"
#include "libavutil/imgutils.h"
//#include "libavutil/libm.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/eval.h"
#include "libavutil/dict.h"
#include "libavutil/opt.h"
#include "libavutil/cpu.h"
#include "libavutil/ffversion.h"
#include "libavutil/version.h"
#include "libavcodec/bsf.h"

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
#include "libswscale/swscale.h"

extern SDL_Window* window;
extern SDL_Renderer* renderer;
extern SDL_RendererInfo renderer_info;
extern SDL_AudioDeviceID audio_dev;

extern const char program_name[];
extern int default_width;
extern int default_height;

int InitSDL(int audio_disable, int display_disable, int alwaysontop, int borderless, SDL_RendererInfo renderer_info);



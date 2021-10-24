#include "ParseFile.h"

ParseFile::ParseFile(string fileNmae)
	:m_fileName(fileNmae),
	m_formatCtx(NULL),
	m_videoPacket(NULL),
	m_videoIndex(-1),
	m_rawFrame(NULL),
	m_yuvFrame(NULL),
	m_codecCtx(NULL),
	m_data(NULL),
	m_swsContext(NULL)
{

}

void ParseFile::OpenFile()
{
	avformat_open_input(&m_formatCtx, m_fileName.c_str(), NULL, NULL);
}

void ParseFile::FindStream()
{
	avformat_find_stream_info(m_formatCtx, NULL);
}

void ParseFile::LocateVideoStream()
{
	for (int i = 0; i < m_formatCtx->nb_streams; i++)
	{
		if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			m_videoIndex = i;
			break;
		}
	}
}

void ParseFile::CreateCodecContext()
{
	AVCodecParameters* avPar = m_formatCtx->streams[m_videoIndex]->codecpar;
	AVCodec* codec = const_cast<AVCodec*>(avcodec_find_decoder(avPar->codec_id));
	m_codecCtx = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(m_codecCtx, avPar);
	avcodec_open2(m_codecCtx, codec, NULL);
}

void ParseFile::InitFrame()
{
	m_rawFrame = av_frame_alloc();
	m_yuvFrame = av_frame_alloc();
	int size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, m_codecCtx->width, m_codecCtx->height, 1);
	m_data = new uint8_t[size];
	av_image_fill_arrays(m_yuvFrame->data, m_yuvFrame->linesize, m_data, AV_PIX_FMT_YUV420P, m_codecCtx->width, m_codecCtx->height, 1);
}

void ParseFile::InitSwScaleContext()
{
	m_swsContext = sws_getContext(m_codecCtx->width, m_codecCtx->height, m_codecCtx->pix_fmt, m_codecCtx->width, m_codecCtx->height, AV_PIX_FMT_YUV420P,
		SWS_BICUBIC, NULL, NULL, NULL);
}

void ParseFile::AllocatePacket()
{
	m_videoPacket = new AVPacket();
}

void ParseFile::ReadData()
{
	while (av_read_frame(m_formatCtx, m_videoPacket))
	{
		if (m_videoPacket->stream_index == m_videoIndex)
		{
			DecodeData();
			ConvertVideo();
		}
		av_packet_unref(m_videoPacket);
	}
}

void ParseFile::DecodeData()
{
	avcodec_send_packet(m_codecCtx, m_videoPacket);
	avcodec_receive_frame(m_codecCtx, m_rawFrame);
}

void ParseFile::ConvertVideo()
{
	sws_scale(m_swsContext, m_rawFrame->data, m_rawFrame->linesize, 0, m_codecCtx->height, m_yuvFrame->data, m_yuvFrame->linesize);
}

void ParseFile::Close()
{
	sws_freeContext(m_swsContext);
	delete[] m_data;
	av_frame_free(&m_rawFrame);
	av_frame_free(&m_yuvFrame);
	avcodec_close(m_codecCtx);
	avformat_close_input(&m_formatCtx);
}

int ParseFile::GetWidth()
{
	if (m_codecCtx)
	{
		return m_codecCtx->width;
	}
	return 0;
}

int ParseFile::GetHeight()
{
	if (m_codecCtx)
	{
		return m_codecCtx->height;
	}
	return 0;
}

AVFrame* ParseFile::GetFrame()
{
	return m_yuvFrame;
}

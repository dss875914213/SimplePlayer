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

bool ParseFile::Init()
{
	bool ret;
	ret = OpenFile();
	if (ret <= 0)
	{
		cout << "Failed to OpenFile" << endl;
		return false;
	}
	ret = FindStream();
	if (ret <= 0)
	{
		cout << "Failed to FindStream" << endl;
		return false;
	}
	ret = LocateVideoStream();
	if (ret <= 0)
	{
		cout << "Failed to LocateVideoStream" << endl;
		return false;
	}
	ret = CreateCodecContext();
	if (ret <= 0)
	{
		cout << "Failed to CreateCodecContext" << endl;
		return false;
	}
	ret = InitFrame();
	if (ret <= 0)
	{
		cout << "Failed to InitFrame" << endl;
		return false;
	}
	ret = InitSwScaleContext();
	if (ret <= 0)
	{
		cout << "Failed to InitSwSacleContext" << endl;
		return false;
	}
	ret = AllocatePacket();
	if (ret <= 0)
	{
		cout << "Failed to AllocatePacket" << endl;
		return false;
	}
	return true;
}

bool ParseFile::OpenFile()
{
	int ret;
	ret = avformat_open_input(&m_formatCtx, m_fileName.c_str(), NULL, NULL);
	return ret >= 0 ? true: false;
}

bool ParseFile::FindStream()
{
	int ret;
	ret = avformat_find_stream_info(m_formatCtx, NULL);
	return ret >= 0 ? true : false;
}

bool ParseFile::LocateVideoStream()
{
	for (int i = 0; i < m_formatCtx->nb_streams; i++)
	{
		if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			m_videoIndex = i;
			break;
		}
	}
	return true;
}

bool ParseFile::CreateCodecContext()
{
	int ret;
	AVCodecParameters* avPar = m_formatCtx->streams[m_videoIndex]->codecpar;
	AVCodec* codec = const_cast<AVCodec*>(avcodec_find_decoder(avPar->codec_id));
	if (codec == NULL)
	{
		cout << "Can't find codec!" << endl;
		return false;
	}
	m_codecCtx = avcodec_alloc_context3(codec);
	if (m_codecCtx == NULL)
	{
		cout << "Can't alloc AVcodec context" << endl;
		return false;
	}
	ret = avcodec_parameters_to_context(m_codecCtx, avPar);
	if (ret < 0)
	{
		cout << "Failed to avcodec_parameters_to_context" << endl;
		return false;
	}
	ret = avcodec_open2(m_codecCtx, codec, NULL);
	if (ret < 0)
	{
		cout << "Failed to avcodec2_open2" << endl;
		return false;
	}
	return true;
}

bool ParseFile::InitFrame()
{
	m_rawFrame = av_frame_alloc();
	m_yuvFrame = av_frame_alloc();
	if (!m_rawFrame|| !m_yuvFrame)
	{
		cout << "Failed to av_frame_alloc" << endl;
		return false;
	}
	int size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, m_codecCtx->width, m_codecCtx->height, 1);
	m_data = new uint8_t[size];
	av_image_fill_arrays(m_yuvFrame->data, m_yuvFrame->linesize, m_data, AV_PIX_FMT_YUV420P, m_codecCtx->width, m_codecCtx->height, 1);
	return true;
}

bool ParseFile::InitSwScaleContext()
{
	m_swsContext = sws_getContext(m_codecCtx->width, m_codecCtx->height, m_codecCtx->pix_fmt, m_codecCtx->width, m_codecCtx->height, AV_PIX_FMT_YUV420P,
		SWS_BICUBIC, NULL, NULL, NULL);
	return true;
}

bool ParseFile::AllocatePacket()
{
	m_videoPacket = new AVPacket();
	return true;
}

bool ParseFile::ReadData()
{
	bool ret = false;
	if (av_read_frame(m_formatCtx, m_videoPacket)==0)
	{
		if (m_videoPacket->stream_index == m_videoIndex)
		{
			DecodeData();
			ConvertVideo();
			ret = true;
		} 
		av_packet_unref(m_videoPacket);
	}
	return ret;
}

bool ParseFile::DecodeData()
{
	avcodec_send_packet(m_codecCtx, m_videoPacket);
	avcodec_receive_frame(m_codecCtx, m_rawFrame);
	return true;
}

bool ParseFile::ConvertVideo()
{
	sws_scale(m_swsContext, m_rawFrame->data, m_rawFrame->linesize, 0, m_codecCtx->height, m_yuvFrame->data, m_yuvFrame->linesize);
	return true;
}

void ParseFile::Close()
{
	sws_freeContext(m_swsContext);
	delete[] m_data;
	delete[] m_videoPacket;
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

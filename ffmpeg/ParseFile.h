#pragma once
#include <iostream>
extern "C"
{
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
}

#include <string>
using namespace std;


class ParseFile
{
public:
	ParseFile(string fileNmae);

	bool Init();
	bool ReadData();
	void Close();

	int GetWidth();
	int GetHeight();
	AVFrame* GetFrame();

private:
	bool OpenFile();
	bool FindStream();
	bool LocateVideoStream();
	bool CreateCodecContext();
	bool InitFrame();
	bool InitSwScaleContext();
	bool AllocatePacket();

	bool DecodeData();
	bool ConvertVideo();

private:
	string m_fileName;
	AVFormatContext* m_formatCtx;
	AVPacket* m_videoPacket;
	int m_videoIndex;
	AVFrame* m_rawFrame;
	AVFrame* m_yuvFrame;
	AVCodecContext* m_codecCtx;
	SwsContext* m_swsContext;
	uint8_t* m_data;
};


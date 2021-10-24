#include <iostream>
#include "ParseFile.h"
#include "MySDL.h"
extern "C"
{
#include "libavformat/avformat.h"
}
using namespace std;

int main(int argc, char* argv[])
{
	std::cout << "hello world" << std::endl;
	av_log_set_level(AV_LOG_DEBUG);
	ParseFile* parseFile = new ParseFile("C:\\Users\\tianhun\\Videos\\3.mp4");
	parseFile->OpenFile();
	parseFile->FindStream();
	parseFile->LocateVideoStream();
	parseFile->CreateCodecContext();
	parseFile->InitFrame();
	parseFile->InitSwScaleContext();
	parseFile->AllocatePacket();
	MySDL* mySDL = new MySDL(parseFile->GetWidth(), parseFile->GetHeight());
	mySDL->Init();
	while (1)
	{
		parseFile->ReadData();
		parseFile->DecodeData();
		parseFile->ConvertVideo();
		mySDL->Render(parseFile->GetFrame());
	}
	parseFile->Close();
	mySDL->Close();
}












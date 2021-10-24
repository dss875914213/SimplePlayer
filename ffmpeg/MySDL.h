#pragma once

extern "C"
{
#include "libavformat/avformat.h"
#include "SDL/SDL.h"
#include "SDL/SDL_video.h"
#include "SDL/SDL_audio.h"
#include "SDL/SDL_render.h"
}

class MySDL
{
public:
	MySDL(int width, int height);
	void Init();
	void Render(AVFrame* yuvFrame);
	void Close();

private:
	SDL_Window* m_screen;
	int m_width;
	int m_height;
	SDL_Renderer* m_sdlRenderer;
	SDL_Texture* m_sdlTexture;
	SDL_Rect* m_sdlRect;
};


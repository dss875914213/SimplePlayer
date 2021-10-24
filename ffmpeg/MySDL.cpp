#include "MySDL.h"

MySDL::MySDL(int width, int height)
	:m_width(width),
	m_height(height),
	m_sdlRenderer(NULL),
	m_sdlTexture(NULL),
	m_screen(NULL)
{
	m_sdlRect = new SDL_Rect();
}

void MySDL::Init()
{
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);

	m_screen = SDL_CreateWindow("Simplest ffmpeg player's Window",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		m_width, m_height,
		SDL_WINDOW_OPENGL);

	m_sdlRenderer = SDL_CreateRenderer(m_screen, -1, 0);
	m_sdlTexture = SDL_CreateTexture(m_sdlRenderer, SDL_PIXELFORMAT_IYUV,
		SDL_TEXTUREACCESS_STREAMING,
		m_width, m_height);
	m_sdlRect->x = 0;
	m_sdlRect->y = 0;
	m_sdlRect->w = m_width;
	m_sdlRect->h = m_height;
}

void MySDL::Render(AVFrame* yuvFrame)
{
	SDL_UpdateYUVTexture(m_sdlTexture,
		m_sdlRect,
		yuvFrame->data[0],
		yuvFrame->linesize[0],
		yuvFrame->data[1],
		yuvFrame->linesize[1],
		yuvFrame->data[2],
		yuvFrame->linesize[2]);
	SDL_RenderClear(m_sdlRenderer);
	SDL_RenderCopy(m_sdlRenderer, m_sdlTexture,
		NULL, m_sdlRect);
	SDL_RenderPresent(m_sdlRenderer);
	SDL_Delay(40);
}

void MySDL::Close()
{
	SDL_Quit();
}

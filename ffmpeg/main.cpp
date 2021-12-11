#include <iostream>
#include "ParseFile.h"
#include "MySDL.h"
#include <windows.h>
using namespace std;

HANDLE pProcess;

DWORD WINAPI ThreadFunc(PVOID pvParam)
{
	while (1)
	{
		SetEvent(pProcess);
		Sleep(40);
	}
	return 0;
}


int main(int argc, char* argv[])
{
	ParseFile* parseFile = new ParseFile("C:\\Users\\tianhun\\Videos\\3.mp4");
	pProcess = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!parseFile->Init())
	{
		cout << "Failed to Init parseFile" << endl;
		return -1;
	}
	CreateThread(NULL, 0, ThreadFunc, NULL, 0, NULL);
	MySDL* mySDL = new MySDL(parseFile->GetWidth(), parseFile->GetHeight());
	mySDL->Init();
	while (1)
	{
		WaitForSingleObject(pProcess, INFINITE);
		if (parseFile->ReadData())
		{
			mySDL->Render(parseFile->GetFrame());
		}
		ResetEvent(pProcess);
	}
	parseFile->Close();
	mySDL->Close();
}












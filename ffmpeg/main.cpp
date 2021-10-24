#include <iostream>
#include "ParseFile.h"
#include "MySDL.h"
using namespace std;

int main(int argc, char* argv[])
{
	ParseFile* parseFile = new ParseFile("C:\\Users\\tianhun\\Videos\\3.mp4");

	if (!parseFile->Init())
	{
		cout << "Failed to Init parseFile" << endl;
		return -1;
	}
	MySDL* mySDL = new MySDL(parseFile->GetWidth(), parseFile->GetHeight());
	mySDL->Init();
	while (1)
	{
		if (parseFile->ReadData())
		{
			mySDL->Render(parseFile->GetFrame());
		}
		mySDL->Delay();
	}
	parseFile->Close();
	mySDL->Close();
}












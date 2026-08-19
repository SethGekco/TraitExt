#include <Phobos.h>

// Static member definitions required by Phobos utility sources we compile
// (Debug/Stream/Patch). Minimal stub — only what this project needs.

HANDLE Phobos::hInstance = nullptr;

char Phobos::readBuffer[Phobos::readLength];
wchar_t Phobos::wideBuffer[Phobos::readLength];

void Phobos::CmdLineParse(char**, int) { }
void Phobos::ExeRun() { }
void Phobos::ExeTerminate() { }

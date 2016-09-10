/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Windows Client
 *
 * Copyright 2009-2011 Jay Sorg
 * Copyright 2010-2011 Vic Lee
 * Copyright 2010-2011 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Myrtille: A native HTML4/5 Remote Desktop Protocol client
 *
 * Copyright 2014-2016 Cedric Coste
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma region Myrtille

#include <string>
#include <sstream>
#include <vector>

#include <objidl.h>

#include <GdiPlus.h>
#pragma comment(lib, "gdiplus")
using namespace Gdiplus;

 //#include "webp/encode.h"
 //#pragma comment(lib, "libwebp.lib")
 //const char* webpFilename;	// debug

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

#include "wf_client.h"
#include "wf_myrtille.h"

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid);
std::string getCurrentTime();
std::string createLogDirectory();
std::wstring s2ws(const std::string& s);
DWORD connectRemoteSessionPipes(wfContext* wfc);
HANDLE connectRemoteSessionPipe(wfContext* wfc, std::string pipeName, DWORD accessMode);
std::string createRemoteSessionDirectory(wfContext* wfc);
void SendMouseInput(wfContext* wfc, std::string input, int positionIdx, UINT16 flags);
void processImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int left, int top, int right, int bottom, bool fullscreen);
void saveImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, int format, int quality, bool fullscreen);
void sendImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, int posX, int posY, int width, int height, int format, int quality, IStream* stream, int size, bool fullscreen);
char* unbase64(char* b64buffer, int length);

//void WebPEncoder(wfContext* wfc, Gdiplus::Bitmap* bmp, IStream* stream, float quality, bool fullscreen);
//static int WebPWriter(const uint8_t* data, size_t data_size, const WebPPicture* const pic);

DWORD WINAPI ProcessInputsPipe(LPVOID lpParameter);
DWORD WINAPI SendFullscreen(LPVOID lpParameter);

#define TAG CLIENT_TAG("myrtille")

#define INPUTS_PIPE_BUFFER_SIZE 131072		// 128 KB
#define UPDATES_PIPE_BUFFER_SIZE 1048576	// 1024 KB
#define MAX_IMAGE_COUNT_PER_SECOND 0		// ips throttling; pros: less images to generate = lower server cpu / faster; cons: skipping some images may result in display inconsistencies. 0 to disable

// rdp command
enum RDP_COMMAND
{
	RDP_COMMAND_SEND_FULLSCREEN_UPDATE = 0,
	RDP_COMMAND_CLOSE_RDP_CLIENT = 1,
	RDP_COMMAND_SET_IMAGE_ENCODING = 2,
	RDP_COMMAND_SET_IMAGE_QUALITY = 3,
	RDP_COMMAND_REQUEST_REMOTE_CLIPBOARD = 4
};

// image encoding
enum IMAGE_ENCODING
{
	IMAGE_ENCODING_PNG = 0,
	IMAGE_ENCODING_JPEG = 1,	// default
	IMAGE_ENCODING_PNG_JPEG = 2,
	IMAGE_ENCODING_WEBP = 3
};

// image format
enum IMAGE_FORMAT
{
	IMAGE_FORMAT_PNG = 0,
	IMAGE_FORMAT_JPEG = 1,
	IMAGE_FORMAT_WEBP = 2,
	IMAGE_FORMAT_CUR = 3
};

// image quality (%)
// fact is, it may vary depending on the image format...
// to keep things easy, and because there are only 2 quality based (lossy) formats managed by this program (JPEG and WEBP... PNG is lossless), we use the same * base * values for all of them...
enum IMAGE_QUALITY
{
	IMAGE_QUALITY_LOW = 10,
	IMAGE_QUALITY_MEDIUM = 25,
	IMAGE_QUALITY_HIGH = 50,	// default; may be tweaked dynamically depending on image encoding and client bandwidth
	IMAGE_QUALITY_HIGHER = 75,	// used for fullscreen updates
	IMAGE_QUALITY_HIGHEST = 100
};

struct wf_myrtille
{
	// pipes
	HANDLE inputsPipe;
	HANDLE updatesPipe;

	// inputs
	bool processInputs;

	// updates
	int imageEncoding = IMAGE_ENCODING_JPEG;
	int imageQuality = IMAGE_QUALITY_HIGH;
	int imageIdx = 0;
	DWORD imageCountLastCheck = 0;
	int imageCountPerSec = 0;

	// clipboard
	std::string clipboardText;
	bool clipboardUpdated;

	// GDI+
	ULONG_PTR gdiplusToken;
	CLSID pngClsid;
	CLSID jpgClsid;
	EncoderParameters encoderParameters;

	// WebP
	//WebPConfig webpConfig;
};
typedef struct wf_myrtille wfMyrtille;

void wf_myrtille_start(wfContext* wfc)
{
	if (wfc->settings->MyrtilleSessionId == 0)
		return;

	wfc->myrtille = (wfMyrtille*)calloc(1, sizeof(wfMyrtille));
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	#if !defined(WITH_DEBUG) && !defined(_DEBUG)
	// by default, redirect stdout to nothing (same as linux "/dev/null")
	freopen("nul", "w", stdout);
	#endif

	// debug
	if (wfc->settings->MyrtilleDebugLog)
	{
		std::string logDirectoryPath = createLogDirectory();
		if (logDirectoryPath != "")
		{
			std::stringstream ss;
			ss << logDirectoryPath << "\\wfreerdp." << GetCurrentProcessId() << ".log";
			std::string s = ss.str();
			const char* logFilename = s.c_str();
			freopen(logFilename, "w", stdout);
			freopen(logFilename, "w", stderr);
		}
	}

	// clipboard
	myrtille->clipboardText = "clipboard|";
	myrtille->clipboardUpdated = false;

	// GDI+
	GdiplusStartupInput gdiplusStartupInput;
	GdiplusStartup(&myrtille->gdiplusToken, &gdiplusStartupInput, NULL);

	GetEncoderClsid(L"image/png", &myrtille->pngClsid);
	GetEncoderClsid(L"image/jpeg", &myrtille->jpgClsid);

	int quality = IMAGE_QUALITY_HIGH;
	EncoderParameters encoderParameters;
	encoderParameters.Count = 1;
	encoderParameters.Parameter[0].Guid = EncoderQuality;
	encoderParameters.Parameter[0].Type = EncoderParameterValueTypeLong;
	encoderParameters.Parameter[0].NumberOfValues = 1;
	encoderParameters.Parameter[0].Value = &quality;

	myrtille->encoderParameters = encoderParameters;

	// WebP
	//float webpQuality = IMAGE_QUALITY_HIGH;
	//WebPConfig webpConfig;
	//WebPConfigPreset(&webpConfig, WEBP_PRESET_PICTURE, webpQuality);

	// override preset settings below, if needed

	//webpConfig.quality = webpQuality;
	//webpConfig.target_size = 0;
	//webpConfig.target_PSNR = 0.;
	//webpConfig.method = 3;
	//webpConfig.sns_strength = 30;
	//webpConfig.filter_strength = 20;
	//webpConfig.filter_sharpness = 3;
	//webpConfig.filter_type = 0;
	//webpConfig.partitions = 0;
	//webpConfig.segments = 2;
	//webpConfig.pass = 1;
	//webpConfig.show_compressed = 0;
	//webpConfig.preprocessing = 0;
	//webpConfig.autofilter = 0;
	//webpConfig.alpha_compression = 0;
	//webpConfig.partition_limit = 0;

	//myrtille->webpConfig = webpConfig;
}

void wf_myrtille_stop(wfContext* wfc)
{
	if (wfc->settings->MyrtilleSessionId == 0)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;
	myrtille->processInputs = false;
}

void wf_myrtille_connect(wfContext* wfc)
{
	if (wfc->settings->MyrtilleSessionId == 0)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// connect pipes
	DWORD result = connectRemoteSessionPipes(wfc);
	if (result != 0)
	{
		WLog_ERR(TAG, "wf_myrtille_connect: failed to connect session %i with error %d", wfc->settings->MyrtilleSessionId, result);
		return;
	}

	WLog_INFO(TAG, "wf_myrtille_connect: connected session %i", wfc->settings->MyrtilleSessionId);

	// process inputs
	DWORD threadId;
	if (CreateThread(NULL, 0, ProcessInputsPipe, (void*)wfc, 0, &threadId) == NULL)
	{
		WLog_ERR(TAG, "wf_myrtille_connect: CreateThread failed for inputs pipe with error %d", GetLastError());
		return;
	}

	// handshaking
	CHAR hello[] = "Hello server";
	DWORD bytesToWrite = 12;
	DWORD bytesWritten;
	if (WriteFile(myrtille->updatesPipe, hello, bytesToWrite, &bytesWritten, NULL) == 0)
	{
		WLog_ERR(TAG, "wf_myrtille_connect: handshaking failed with error %d", GetLastError());
	}
}

void wf_myrtille_send_region(wfContext* wfc, RECT region)
{
	if (wfc->settings->MyrtilleSessionId == 0)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// --------------------------- consistency check ----------------------------------------------

	if (region.left < 0 || region.left > wfc->width || region.top < 0 || region.top > wfc->height ||
		region.right < 0 || region.right > wfc->width || region.bottom < 0 || region.bottom > wfc->height ||
		region.left > region.right || region.top > region.bottom)
		return;

	// --------------------------- ips regulator --------------------------------------------------

	if (MAX_IMAGE_COUNT_PER_SECOND > 0)
	{
		DWORD now = GetTickCount();

		if (myrtille->imageCountLastCheck == 0)
			myrtille->imageCountLastCheck = now;

		int diff = now - myrtille->imageCountLastCheck;

		if (diff >= 1000)
		{
			myrtille->imageCountLastCheck = now;
			myrtille->imageCountPerSec = 0;
		}
		else
			myrtille->imageCountPerSec++;

		if (myrtille->imageCountPerSec >= MAX_IMAGE_COUNT_PER_SECOND)
			return;
	}

	// --------------------------- extract the consolidated region --------------------------------

	HDC hdc = CreateCompatibleDC(wfc->primary->hdc);
	HBITMAP hbmp = CreateCompatibleBitmap(wfc->primary->hdc, region.right - region.left, region.bottom - region.top);
	SelectObject(hdc, hbmp);

	// debug, if needed
	//WLog_INFO(TAG, "wf_myrtille_send_region: left:%i, top:%i, right:%i, bottom:%i", region.left, region.top, region.right, region.bottom);

	BitBlt(
		hdc,
		0,
		0,
		region.right - region.left,
		region.bottom - region.top,
		wfc->primary->hdc,
		region.left,
		region.top,
		SRCCOPY);

	Gdiplus::Bitmap *bmpRegion = Gdiplus::Bitmap::FromHBITMAP(hbmp, (HPALETTE)0);

	// ---------------------------  process it ----------------------------------------------------

	processImage(wfc, bmpRegion, region.left, region.top, region.right, region.bottom, false);

	// ---------------------------  cleanup -------------------------------------------------------

	delete bmpRegion;
	bmpRegion = NULL;

	DeleteObject(hbmp);
	hbmp = NULL;

	DeleteDC(hdc);
	hdc = NULL;
}

void wf_myrtille_send_cursor(wfContext* wfc)
{
	if (wfc->settings->MyrtilleSessionId == 0)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// --------------------------- set cursor -----------------------------------------------------

	// set a display context and a bitmap to store the mouse cursor
	HDC hdc = CreateCompatibleDC(wfc->primary->hdc);
	HBITMAP hbmp = CreateCompatibleBitmap(wfc->primary->hdc, GetSystemMetrics(SM_CXCURSOR), GetSystemMetrics(SM_CYCURSOR));
	SelectObject(hdc, hbmp);

	// set a colored background, so that it will be possible to identify parts of the cursor that should be made transparent
	HBRUSH hbrush = CreateSolidBrush(RGB(0, 0, 255));

	// draw the cursor on the display context
	DrawIconEx(hdc, 0, 0, (HICON)wfc->cursor, 0, 0, 0, hbrush, DI_NORMAL);

	// cursor bitmap
	Gdiplus::Bitmap *bmpCursor = Gdiplus::Bitmap::FromHBITMAP(hbmp, (HPALETTE)0);

	// extract the relevant cursor image. also, transparency requires ARGB format
	Gdiplus::Bitmap *bmpTransparentCursor = bmpCursor->Clone(
		0,
		0,
		GetSystemMetrics(SM_CXCURSOR),
		GetSystemMetrics(SM_CYCURSOR),
		PixelFormat32bppARGB);

	// make the background transparent
	for (int x = 0; x < bmpTransparentCursor->GetWidth(); x++)
	{
		for (int y = 0; y < bmpTransparentCursor->GetHeight(); y++)
		{
			Gdiplus::Color color;
			bmpTransparentCursor->GetPixel(x, y, &color);

			if (color.GetValue() == Gdiplus::Color::Blue)
			{
				bmpTransparentCursor->SetPixel(x, y, Gdiplus::Color::Transparent);
			}

			// for some reason, some cursors (like the text one) are yellow instead of black ?! switching color...
			else if (color.GetValue() == Gdiplus::Color::Yellow)
			{
				bmpTransparentCursor->SetPixel(x, y, Gdiplus::Color::Black);
			}
		}
	}

	// convert into PNG
	IStream* pngStream;
	CreateStreamOnHGlobal(NULL, TRUE, &pngStream);
	bmpTransparentCursor->Save(pngStream, &myrtille->pngClsid);

	STATSTG statstg;
	pngStream->Stat(&statstg, STATFLAG_DEFAULT);
	ULONG pngSize = (ULONG)statstg.cbSize.LowPart;

	// retrieve cursor info
	ICONINFO cursorInfo;
	GetIconInfo((HICON)wfc->cursor, &cursorInfo);

	// send
	if (pngStream != NULL && pngSize > 0 && pngSize <= UPDATES_PIPE_BUFFER_SIZE)
	{
		sendImage(
			wfc,
			bmpTransparentCursor,
			++myrtille->imageIdx,
			cursorInfo.xHotspot,
			cursorInfo.yHotspot,
			bmpTransparentCursor->GetWidth(),
			bmpTransparentCursor->GetHeight(),
			IMAGE_FORMAT_CUR,
			IMAGE_QUALITY_HIGHEST,
			pngStream,
			pngSize,
			false);
	}

	// cleanup
	DeleteObject(cursorInfo.hbmMask);
	DeleteObject(cursorInfo.hbmColor);

	if (pngStream != NULL)
	{
		pngStream->Release();
		pngStream = NULL;
	}

	delete bmpTransparentCursor;
	bmpTransparentCursor = NULL;

	delete bmpCursor;
	bmpCursor = NULL;

	DeleteObject(hbrush);
	hbrush = NULL;

	DeleteObject(hbmp);
	hbmp = NULL;

	DeleteDC(hdc);
	hdc = NULL;
}

void wf_myrtille_reset_clipboard(wfContext* wfc)
{
	if (wfc->settings->MyrtilleSessionId == 0)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;
	
	myrtille->clipboardText = "clipboard|";
	myrtille->clipboardUpdated = true;
}

void wf_myrtille_send_clipboard(wfContext* wfc, BYTE* data, UINT32 length)
{
	if (wfc->settings->MyrtilleSessionId == 0)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	std::stringstream ss;
	ss << "clipboard|";
	for (int i = 0; i < length; i++)
	{
		if (data[i] != '\0')
			ss << data[i];
	}
	
	myrtille->clipboardText = ss.str();
	myrtille->clipboardUpdated = false;

	DWORD bytesWritten;
	if (WriteFile(myrtille->updatesPipe, myrtille->clipboardText.c_str(), myrtille->clipboardText.length(), &bytesWritten, NULL) == 0)
	{
		WLog_ERR(TAG, "ProcessInputsPipe: WriteFile failed for clipboard with error %d", GetLastError());
	}
}

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
	UINT  num = 0;          // number of image encoders
	UINT  size = 0;         // size of the image encoder array in bytes

	ImageCodecInfo* pImageCodecInfo = NULL;

	GetImageEncodersSize(&num, &size);
	if (size == 0)
		return -1;  // Failure

	pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
	if (pImageCodecInfo == NULL)
		return -1;  // Failure

	GetImageEncoders(num, size, pImageCodecInfo);

	for (UINT j = 0; j < num; ++j)
	{
		if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0)
		{
			*pClsid = pImageCodecInfo[j].Clsid;
			free(pImageCodecInfo);
			return j;  // Success
		}
	}

	free(pImageCodecInfo);
	return -1;  // Failure
}

std::string getCurrentTime()
{
	SYSTEMTIME time;
	GetLocalTime(&time);

	WORD year = time.wYear;
	WORD month = time.wMonth;
	WORD day = time.wDay;
	WORD hour = time.wHour;
	WORD minute = time.wMinute;
	WORD second = time.wSecond;
	WORD millisecond = time.wMilliseconds;

	// YYYY-MM-DD hh:mm:ss,fff
	std::stringstream ss;
	ss << year << "-" <<
		(month < 10 ? "0" : "") << month << "-" <<
		(day < 10 ? "0" : "") << day << " " <<
		(hour < 10 ? "0" : "") << hour << ":" <<
		(minute < 10 ? "0" : "") << minute << ":" <<
		(second < 10 ? "0" : "") << second << "," <<
		(millisecond < 100 ? (millisecond < 10 ? "00" : "0") : "") << millisecond;

	return ss.str();
}

std::string createLogDirectory()
{
	std::string path = "";

	// retrieve the module file name
	wchar_t* buffer = new wchar_t[MAX_PATH];
	if (GetModuleFileName(NULL, buffer, MAX_PATH))
	{
		// extract the parent folder
		char moduleFilename[MAX_PATH];
		wcstombs(moduleFilename, buffer, MAX_PATH);
		std::string::size_type pos = std::string(moduleFilename).find_last_of("\\/");
		std::string currentdir = std::string(moduleFilename).substr(0, pos);
		pos = currentdir.find_last_of("\\/");
		std::string parentdir = currentdir.substr(0, pos);

		// log folder
		std::stringstream ss;
		ss << parentdir << "\\log";
		path = ss.str();
		std::wstring ws = s2ws(path);
		LPCWSTR logDir = ws.c_str();

		// create the log folder if not already exists
		if (!CreateDirectory(logDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
		{
			WLog_ERR(TAG, "createLogDirectory: create directory failed with error %d", GetLastError());
			path = "";
		}
	}
	else
	{
		WLog_ERR(TAG, "createLogDirectory: can't retrieve the module filename %d", GetLastError());
	}

	// cleanup
	delete[] buffer;

	return path;
}

std::wstring s2ws(const std::string& s)
{
	int len;
	int slength = (int)s.length() + 1;
	len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, 0, 0);
	wchar_t* buf = new wchar_t[len];
	MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, buf, len);
	std::wstring r(buf);
	delete[] buf;
	return r;
}

DWORD connectRemoteSessionPipes(wfContext* wfc)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// connect inputs pipe (user inputs and rdp commands)
	if ((myrtille->inputsPipe = connectRemoteSessionPipe(wfc, "inputs", GENERIC_READ)) == INVALID_HANDLE_VALUE)
	{
		WLog_ERR(TAG, "connectRemoteSessionPipes: connect failed for inputs pipe with error %d", GetLastError());
		return GetLastError();
	}

	// connect updates pipe (region and fullscreen updates)
	if ((myrtille->updatesPipe = connectRemoteSessionPipe(wfc, "updates", GENERIC_WRITE)) == INVALID_HANDLE_VALUE)
	{
		WLog_ERR(TAG, "connectRemoteSessionPipes: connect failed for updates pipe with error %d", GetLastError());
		return GetLastError();
	}

	return 0;
}

HANDLE connectRemoteSessionPipe(wfContext* wfc, std::string pipeName, DWORD accessMode)
{
	std::stringstream ss;
	ss << "\\\\.\\pipe\\remotesession_" << wfc->settings->MyrtilleSessionId << "_" << pipeName;
	std::string s = ss.str();
	std::wstring ws = s2ws(s);
	LPCWSTR pipeFileName = ws.c_str();

	return CreateFile(pipeFileName, accessMode, 0, NULL, OPEN_EXISTING, 0, NULL);
}

std::string createRemoteSessionDirectory(wfContext* wfc)
{
	std::string path = "";

	std::string logDirectoryPath = createLogDirectory();
	if (logDirectoryPath != "")
	{
		std::stringstream ss;
		ss << logDirectoryPath << "\\remotesession_" << wfc->settings->MyrtilleSessionId << "." << GetCurrentProcessId();
		path = ss.str();
		std::wstring ws = s2ws(path);
		LPCWSTR remoteSessionDir = ws.c_str();

		if (!CreateDirectory(remoteSessionDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
		{
			WLog_ERR(TAG, "createRemoteSessionDirectory: CreateDirectory failed with error %d", GetLastError());
			path = "";
		}
	}

	return path;
}

std::vector<std::string> &split(const std::string &s, char delim, std::vector<std::string> &elems)
{
	std::stringstream ss(s);
	std::string item;
	while (std::getline(ss, item, delim)) {
		elems.push_back(item);
	}
	return elems;
}

std::vector<std::string> split(const std::string &s, char delim)
{
	std::vector<std::string> elems;
	return split(s, delim, elems);
}

DWORD WINAPI ProcessInputsPipe(LPVOID lpParameter)
{
	wfContext* wfc = (wfContext*)lpParameter;
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// main loop
	myrtille->processInputs = true;
	while (myrtille->processInputs)
	{
		CHAR buffer[INPUTS_PIPE_BUFFER_SIZE];
		DWORD bytesRead;

		// wait for inputs pipe event
		if (ReadFile(myrtille->inputsPipe, buffer, INPUTS_PIPE_BUFFER_SIZE, &bytesRead, NULL) == 0)
		{
			switch (GetLastError())
			{
				case ERROR_INVALID_HANDLE:
					WLog_ERR(TAG, "ProcessInputsPipe: ReadFile failed with error ERROR_INVALID_HANDLE");
					break;

				case ERROR_PIPE_NOT_CONNECTED:
					WLog_ERR(TAG, "ProcessInputsPipe: ReadFile failed with error ERROR_PIPE_NOT_CONNECTED");
					break;

				case ERROR_PIPE_BUSY:
					WLog_ERR(TAG, "ProcessInputsPipe: ReadFile failed with error ERROR_PIPE_BUSY");
					break;

				case ERROR_BAD_PIPE:
					WLog_ERR(TAG, "ProcessInputsPipe: ReadFile failed with error ERROR_BAD_PIPE");
					break;

				case ERROR_BROKEN_PIPE:
					WLog_ERR(TAG, "ProcessInputsPipe: ReadFile failed with error ERROR_BROKEN_PIPE");
					break;

				default:
					WLog_ERR(TAG, "ProcessInputsPipe: ReadFile failed with error %d", GetLastError());
					break;
			}

			// pipe problem; exit
			myrtille->processInputs = false;
		}
		else
		{
			std::string message(buffer, bytesRead);

			WLog_INFO(TAG, "ProcessInputsPipe: ReadFile succeeded: %s", message.c_str());

			if (bytesRead > 0)
			{
				std::vector<std::string> inputs = split(message, ',');

				for (int i = 0; i < inputs.size(); i++)
				{
					std::string input = inputs[i];
					std::string inputType = input.substr(0, 1);

					// command
					if (inputType == "C")
					{
						int separatorIdx = input.find("-");
						if (separatorIdx != std::string::npos)
						{
							std::string cmdCode = input.substr(1, separatorIdx - 1);
							std::string cmdArgs = input.substr(separatorIdx + 1, input.length() - separatorIdx - 1);

							switch (stoi(cmdCode))
							{
								// fullscreen update request
								case RDP_COMMAND_SEND_FULLSCREEN_UPDATE:
									if (QueueUserWorkItem(SendFullscreen, lpParameter, WT_EXECUTEDEFAULT) == 0)
										WLog_ERR(TAG, "ProcessInputsPipe: QueueUserWorkItem failed for SendFullscreen with error %d", GetLastError());
									break;

								// the standard way to close an rdp session is to logoff the user; an alternate way is to simply close the rdp client
								// this disconnect the session, which is then subsequently closed (1 sec later if "MaxDisconnectionTime" = 1000 ms)
								case RDP_COMMAND_CLOSE_RDP_CLIENT:
									myrtille->processInputs = false;
									break;

								// image encoding
								case RDP_COMMAND_SET_IMAGE_ENCODING:
									myrtille->imageEncoding = stoi(cmdArgs);
									myrtille->imageQuality = IMAGE_QUALITY_HIGH;
									break;

								// image quality is tweaked depending on client bandwidth
								case RDP_COMMAND_SET_IMAGE_QUALITY:
									myrtille->imageQuality = stoi(cmdArgs);
									break;

								// request clipboard value
								case RDP_COMMAND_REQUEST_REMOTE_CLIPBOARD:
									if (myrtille->clipboardUpdated)
									{
										if (!wfc->cliprdr || !wfc->cliprdr->ClientFormatDataRequest)
										{
											WLog_INFO(TAG, "ProcessInputsPipe: clipboard redirect is disabled, request cancelled");
										}
										else
										{
											CLIPRDR_FORMAT_DATA_REQUEST formatDataRequest;
											formatDataRequest.requestedFormatId = CF_UNICODETEXT;
											wfc->cliprdr->ClientFormatDataRequest(wfc->cliprdr, &formatDataRequest);
										}
									}
									else
									{
										DWORD bytesWritten;
										if (WriteFile(myrtille->updatesPipe, myrtille->clipboardText.c_str(), myrtille->clipboardText.length(), &bytesWritten, NULL) == 0)
										{
											WLog_ERR(TAG, "ProcessInputsPipe: WriteFile failed for clipboard with error %d", GetLastError());
										}
									}
									break;
							}
						}
					}

					// keyboard
					else if (inputType == "K" || inputType == "U")
					{
						int separatorIdx = input.find("-");
						if (separatorIdx != std::string::npos)
						{
							std::string keyCode = input.substr(1, separatorIdx - 1);
							std::string pressed = input.substr(separatorIdx + 1, 1);
							if (inputType == "K")
								// keyboard scancode (non character key)
								wfc->instance->input->KeyboardEvent(wfc->instance->input, (pressed == "1" ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE), atoi(keyCode.c_str()));
							else
								// keyboard unicode (character key)
								wfc->instance->input->UnicodeKeyboardEvent(wfc->instance->input, (pressed == "1" ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE), atoi(keyCode.c_str()));
						}
					}

					// mouse
					else if (inputType == "M")
					{
						if (input.length() >= 3)
						{
							if (input.substr(0, 3) == "MMO")
							{
								SendMouseInput(wfc, input, 3, PTR_FLAGS_MOVE);
							}
							else if (input.substr(0, 3) == "MLB")
							{
								UINT16 flags = PTR_FLAGS_BUTTON1;
								if (input.substr(3, 1) == "1")
								{
									flags = PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON1;
								}
								SendMouseInput(wfc, input, 4, flags);
							}
							else if (input.substr(0, 3) == "MMB")
							{
								UINT16 flags = PTR_FLAGS_BUTTON3;
								if (input.substr(3, 1) == "1")
								{
									flags = PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON3;
								}
								SendMouseInput(wfc, input, 4, flags);
							}
							else if (input.substr(0, 3) == "MRB")
							{
								UINT16 flags = PTR_FLAGS_BUTTON2;
								if (input.substr(3, 1) == "1")
								{
									flags = PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON2;
								}
								SendMouseInput(wfc, input, 4, flags);
							}
							else if (input.substr(0, 3) == "MWU")
							{
								SendMouseInput(wfc, input, 3, PTR_FLAGS_WHEEL | 0x0078);
							}
							else if (input.substr(0, 3) == "MWD")
							{
								SendMouseInput(wfc, input, 3, PTR_FLAGS_WHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x0088);
							}
						}
					}
				}
			}
		}
	}

	CloseHandle(myrtille->inputsPipe);
	CloseHandle(myrtille->updatesPipe);
	GdiplusShutdown(myrtille->gdiplusToken);
	exit(EXIT_SUCCESS);

	return 0;
}

void SendMouseInput(wfContext* wfc, std::string input, int positionIdx, UINT16 flags)
{
	int separatorIdx = input.find("-");
	if (separatorIdx != std::string::npos)
	{
		std::string mX = input.substr(positionIdx, separatorIdx - positionIdx);
		std::string mY = input.substr(separatorIdx + 1, input.length() - separatorIdx - 1);
		if (!mX.empty() && stoi(mX) >= 0 && !mY.empty() && stoi(mY) >= 0)
		{
			wfc->instance->input->MouseEvent(wfc->instance->input, flags, stoi(mX), stoi(mY));
		}
	}
}

DWORD WINAPI SendFullscreen(LPVOID lpParameter)
{
	wfContext* wfc = (wfContext*)lpParameter;

	// --------------------------- pipe check -----------------------------------------------------

	if (wfc->settings->MyrtilleSessionId == 0)
		return 0;

	// --------------------------- retrieve the fullscreen bitmap ---------------------------------

	Gdiplus::Bitmap *bmpScreen = Gdiplus::Bitmap::FromHBITMAP(wfc->primary->bitmap, (HPALETTE)0);

	// ---------------------------  process it ----------------------------------------------------

	processImage(wfc, bmpScreen, 0, 0, bmpScreen->GetWidth(), bmpScreen->GetHeight(), true);

	// ---------------------------  cleanup -------------------------------------------------------

	delete bmpScreen;
	bmpScreen = NULL;

	return 0;
}

void processImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int left, int top, int right, int bottom, bool fullscreen)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	IStream* pngStream = NULL;
	IStream* jpgStream = NULL;
	IStream* webpStream = NULL;

	STATSTG statstg;

	int format;
	int quality = (fullscreen ? IMAGE_QUALITY_HIGHER : myrtille->imageQuality);	// use higher quality for fullscreen updates; otherwise current
	IStream* stream = NULL;
	ULONG size = 0;

	/*
	normally, the PNG format is best suited (lower size and better quality) for office applications (with text) and JPG for graphic ones (with images)
	PNG is lossless as opposite to JPG
	WEBP can either be lossy or lossless
	*/

	if (myrtille->imageEncoding == IMAGE_ENCODING_PNG || myrtille->imageEncoding == IMAGE_ENCODING_JPEG || myrtille->imageEncoding == IMAGE_ENCODING_PNG_JPEG)
	{
		ULONG pngSize;
		ULONG jpgSize;

		// --------------------------- convert the bitmap into PNG --------------------------------

		if (myrtille->imageEncoding == IMAGE_ENCODING_PNG || myrtille->imageEncoding == IMAGE_ENCODING_PNG_JPEG)
		{
			CreateStreamOnHGlobal(NULL, TRUE, &pngStream);
			bmp->Save(pngStream, &myrtille->pngClsid);

			pngStream->Stat(&statstg, STATFLAG_DEFAULT);
			pngSize = (ULONG)statstg.cbSize.LowPart;
		}

		// --------------------------- convert the bitmap into JPEG -------------------------------

		if (myrtille->imageEncoding == IMAGE_ENCODING_JPEG || myrtille->imageEncoding == IMAGE_ENCODING_PNG_JPEG)
		{
			CreateStreamOnHGlobal(NULL, TRUE, &jpgStream);
			myrtille->encoderParameters.Parameter[0].Value = &quality;
			bmp->Save(jpgStream, &myrtille->jpgClsid, &myrtille->encoderParameters);

			jpgStream->Stat(&statstg, STATFLAG_DEFAULT);
			jpgSize = (ULONG)statstg.cbSize.LowPart;
		}

		// ---------------------------  use the lowest sized format -------------------------------

		// text, buttons, menus, etc... (simple image structure and low color palette) are more likely to be lower sized in PNG than JPG
		// on the opposite, a complex image (photo or graphical) is more likely to be lower sized in JPG

		if (myrtille->imageEncoding == IMAGE_ENCODING_PNG || (myrtille->imageEncoding == IMAGE_ENCODING_PNG_JPEG && pngSize <= jpgSize))
		{
			stream = pngStream;
			format = IMAGE_FORMAT_PNG;
			quality = IMAGE_QUALITY_HIGHEST;	// lossless
			size = pngSize;
		}
		else
		{
			stream = jpgStream;
			format = IMAGE_FORMAT_JPEG;
			size = jpgSize;
		}
	}
	else if (myrtille->imageEncoding == IMAGE_ENCODING_WEBP)
	{
		// --------------------------- convert the bitmap into WEBP -------------------------------

		CreateStreamOnHGlobal(NULL, TRUE, &webpStream);
		//WebPEncoder(wfc, bmp, webpStream, quality, fullscreen);

		webpStream->Stat(&statstg, STATFLAG_DEFAULT);
		ULONG webpSize = (ULONG)statstg.cbSize.LowPart;

		stream = webpStream;
		format = IMAGE_FORMAT_WEBP;
		size = webpSize;
	}

	// ---------------------------  send the image ------------------------------------------------

	// in order to avoid overloading both the bandwidth and the browser, images are limited to 1024 KB each

	if (stream != NULL && size > 0 && size <= UPDATES_PIPE_BUFFER_SIZE)
	{
		sendImage(
			wfc,
			bmp,
			++myrtille->imageIdx,
			left,
			top,
			right - left,
			bottom - top,
			format,
			quality,
			stream,
			size,
			fullscreen);
	}

	// ---------------------------  cleanup -------------------------------------------------------

	if (pngStream != NULL)
	{
		pngStream->Release();
		pngStream = NULL;
	}

	if (jpgStream != NULL)
	{
		jpgStream->Release();
		jpgStream = NULL;
	}

	if (webpStream != NULL)
	{
		webpStream->Release();
		webpStream = NULL;
	}
}

void saveImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, int format, int quality, bool fullscreen)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	std::string imgDirectoryPath = createRemoteSessionDirectory(wfc);
	if (imgDirectoryPath != "")
	{
		std::stringstream ss;
		ss << imgDirectoryPath;

		switch (format)
		{
		case IMAGE_FORMAT_PNG:
			ss << (fullscreen ? "\\screen_" : "\\region_") << idx << ".png";
			break;

		case IMAGE_FORMAT_JPEG:
			ss << (fullscreen ? "\\screen_" : "\\region_") << idx << "_" << quality << ".jpg";
			break;

		case IMAGE_FORMAT_CUR:
			ss << "\\cursor_" << idx << ".png";
			break;
		}

		std::string s = ss.str();
		std::wstring ws = s2ws(s);
		const wchar_t *filename = ws.c_str();

		switch (format)
		{
		case IMAGE_FORMAT_PNG:
		case IMAGE_FORMAT_CUR:
			bmp->Save(filename, &myrtille->pngClsid);
			break;

		case IMAGE_FORMAT_JPEG:
			myrtille->encoderParameters.Parameter[0].Value = &quality;
			bmp->Save(filename, &myrtille->jpgClsid, &myrtille->encoderParameters);
			break;
		}
	}
}

void sendImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, int posX, int posY, int width, int height, int format, int quality, IStream* stream, int size, bool fullscreen)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// seek to the beginning of the stream
	LARGE_INTEGER li = { 0 };
	stream->Seek(li, STREAM_SEEK_SET, NULL);

	// read the stream into a buffer
	char* buffer = new char[size];
	ULONG bytesRead;
	stream->Read(buffer, size, &bytesRead);

	// encode the buffer to base64
	BIO* bmem, *b64;
	BUF_MEM* bptr;

	b64 = BIO_new(BIO_f_base64());
	bmem = BIO_new(BIO_s_mem());
	b64 = BIO_push(b64, bmem);
	BIO_write(b64, buffer, size);
	BIO_flush(b64);
	BIO_get_mem_ptr(b64, &bptr);

	char* b64buffer = (char*)malloc(bptr->length);
	memcpy(b64buffer, bptr->data, bptr->length - 1);
	b64buffer[bptr->length - 1] = 0;

	// serialize the image
	std::stringstream ss;
	ss << idx << "," << posX << "," << posY << "," << width << "," << height << "," << format << "," << quality << "," << b64buffer << "," << (fullscreen ? "1" : "0");
	std::string s = ss.str();
	const char* imgData = s.c_str();

	DWORD bytesToWrite = s.length();
	DWORD bytesWritten;

	// enqueue it
	if (WriteFile(myrtille->updatesPipe, imgData, bytesToWrite, &bytesWritten, NULL) == 0)
	{
		switch (GetLastError())
		{
		case ERROR_INVALID_HANDLE:
			WLog_ERR(TAG, "ImagesPipe: WriteFile failed with error ERROR_INVALID_HANDLE");
			break;

		case ERROR_PIPE_NOT_CONNECTED:
			WLog_ERR(TAG, "ImagesPipe: WriteFile failed with error ERROR_PIPE_NOT_CONNECTED");
			break;

		case ERROR_PIPE_BUSY:
			WLog_ERR(TAG, "ImagesPipe: WriteFile failed with error ERROR_PIPE_BUSY");
			break;

		case ERROR_BAD_PIPE:
			WLog_ERR(TAG, "ImagesPipe: WriteFile failed with error ERROR_BAD_PIPE");
			break;

		case ERROR_BROKEN_PIPE:
			WLog_ERR(TAG, "ImagesPipe: WriteFile failed with error ERROR_BROKEN_PIPE");
			break;

		default:
			WLog_ERR(TAG, "ImagesPipe: WriteFile failed with error %d", GetLastError());
			break;
		}

		// pipe problem; exit
		myrtille->processInputs = false;
	}

	//WLog_INFO(TAG, "sendImage: WriteFile succeeded for image: %i (%s)", idx, (fullscreen ? "screen" : "region"));

	// images are saved under parent "log\remotesession_#ID.#PID" folder
	//saveImage(wfc, bmp, idx, format, quality, fullscreen);	// debug. enable with caution as it will flood the disk and hinder performance!!!

	// cleanup
	delete[] b64buffer;
	b64buffer = NULL;

	BIO_free_all(b64);

	delete[] buffer;
	buffer = NULL;
}

char* unbase64(char* b64buffer, int length)
{
	char* msg = (char*)malloc(length);
	memset(msg, 0x00, length);

	//WLog_INFO(TAG, "b64buffer: %s", b64buffer);

	BIO* b64, *mem = NULL;

	b64 = BIO_new(BIO_f_base64());
	BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

	mem = BIO_new_mem_buf(b64buffer, length);
	mem = BIO_push(b64, mem);

	int readbytes = -1;
	while ((readbytes = BIO_read(mem, msg, length)) > 0)
	{
		//WLog_INFO(TAG, "readbytes: %d", readbytes);
	}

	BIO_flush(mem);
	//WLog_INFO(TAG, "msg: %s", msg);

	BIO_free_all(mem);
	//BIO_free_all(b64);

	return msg;
}

/*
void WebPEncoder(wfContext* wfc, Gdiplus::Bitmap* bmp, IStream* stream, float quality, bool fullscreen)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	WebPPicture webpPic;

	if (WebPPictureInit(&webpPic))
	{
		// debug

		//if (!createRemoteSessionDirectory(wfc))
		//	return;

		//std::stringstream ss;
		//ss << ".\\remotesession_" << myrtille->settings->MyrtilleSessionId << (fullscreen ? "\\screen_" : "\\region_") << imageIdx << "_" << quality << ".webp";
		//std::string s = ss.str();
		//webpFilename = s.c_str();

		webpPic.custom_ptr = (void*)stream;
		webpPic.writer = WebPWriter;

		webpPic.width = bmp->GetWidth();
		webpPic.height = bmp->GetHeight();

		Gdiplus::BitmapData* bmpData = new Gdiplus::BitmapData();
		Gdiplus::Rect* rect = new Gdiplus::Rect(0, 0, bmp->GetWidth(), bmp->GetHeight());
		bmp->LockBits(rect, ImageLockModeRead, PixelFormat32bppARGB, bmpData);

		const uint8_t* bmpBits = (uint8_t*)bmpData->Scan0;

		if (WebPPictureImportBGRA(&webpPic, bmpBits, bmpData->Stride))
		{
			myrtille->webpConfig.quality = quality;

			if (!WebPEncode(&myrtille->webpConfig, &webpPic))
			WLog_ERR(TAG, "WebpEncode: WebP encoding failed");
		}

		// cleanup

		bmp->UnlockBits(bmpData);

		delete rect;
		rect = NULL;

		delete bmpData;
		bmpData = NULL;

		WebPPictureFree(&webpPic);
	}
}

static int WebPWriter(const uint8_t* data, size_t data_size, const WebPPicture* const pic)
{
	IStream* stream = (IStream*)pic->custom_ptr;

	ULONG bytesWritten;
	stream->Write(data, data_size, &bytesWritten);

	// debug

	//FILE* file = fopen(webpFilename, "ab");
	//if (file != NULL)
	//{
	//	fwrite(data, 1, data_size, file);
	//	fclose(file);
	//}

	return bytesWritten == data_size ? 1 : 0;
}
*/

#pragma endregion
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
 * Copyright(c) 2014-2017 Cedric Coste
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
#include <map>

#include <objidl.h>

#include <GdiPlus.h>
#pragma comment(lib, "gdiplus")
using namespace Gdiplus;

#include "encode.h"
#pragma comment(lib, "libwebp.lib")

#include "wf_client.h"
#include "wf_myrtille.h"

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid);
std::string getCurrentTime();
std::string createLogDirectory();
std::wstring s2ws(const std::string& s);
DWORD connectRemoteSessionPipes(wfContext* wfc);
HANDLE connectRemoteSessionPipe(wfContext* wfc, std::string pipeName, DWORD accessMode);
std::string createRemoteSessionDirectory(wfContext* wfc);
void ProcessMouseInput(wfContext* wfc, std::string input, UINT16 flags);
void processImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int left, int top, int right, int bottom, bool fullscreen);
void saveImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, int format, int quality, bool fullscreen);
void sendImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, int posX, int posY, int width, int height, int format, int quality, IStream* stream, int size, bool fullscreen);
void int32ToBytes(int value, int startIndex, byte* bytes);

void WebPEncoder(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, IStream* stream, float quality, bool fullscreen);
static int WebPWriter(const uint8_t* data, size_t data_size, const WebPPicture* const pic);

DWORD WINAPI ProcessInputsPipe(LPVOID lpParameter);
DWORD WINAPI SendFullscreen(LPVOID lpParameter);
DWORD WINAPI SendReload(LPVOID lpParameter);

#define TAG CLIENT_TAG("myrtille")

#define INPUTS_PIPE_BUFFER_SIZE 131072		// 128 KB
#define UPDATES_PIPE_BUFFER_SIZE 1048576	// 1024 KB
#define IMAGE_COUNT_SAMPLING_RATE 100		// ips sampling (%) less images = lower cpu and bandwidth usage / faster; more = smoother display (skipping images may result in some display inconsistencies). tweaked dynamically to fit the available bandwidth; possible values: 5, 10, 20, 25, 50, 100 (lower = higher drop rate)

// command
enum class COMMAND
{
	// browser
	SEND_BROWSER_RESIZE = 0,

	// keyboard
	SEND_KEY_UNICODE = 1,
	SEND_KEY_SCANCODE = 2,

	// mouse
	SEND_MOUSE_MOVE = 3,
	SEND_MOUSE_LEFT_BUTTON = 4,
	SEND_MOUSE_MIDDLE_BUTTON = 5,
	SEND_MOUSE_RIGHT_BUTTON = 6,
	SEND_MOUSE_WHEEL_UP = 7,
	SEND_MOUSE_WHEEL_DOWN = 8,

	// control
	SET_STAT_MODE = 9,
	SET_DEBUG_MODE = 10,
	SET_COMPATIBILITY_MODE = 11,
	SET_SCALE_DISPLAY = 12,
	SET_IMAGE_ENCODING = 13,
	SET_IMAGE_QUALITY = 14,
	SET_IMAGE_QUANTITY = 15,
	REQUEST_FULLSCREEN_UPDATE = 16,
	REQUEST_REMOTE_CLIPBOARD = 17,
	CLOSE_RDP_CLIENT = 18
};

// command mapping
std::map<std::string, COMMAND> commandMap;

// image encoding
enum class IMAGE_ENCODING
{
	AUTO = 0,								// default
	PNG = 1,
	JPEG = 2,
	WEBP = 3
};

// image format
enum class IMAGE_FORMAT
{
	CUR = 0,
	PNG = 1,
	JPEG = 2,
	WEBP = 3
};

// image quality (%)
// fact is, it may vary depending on the image format...
// to keep things easy, and because there are only 2 quality based (lossy) formats managed by this program (JPEG and WEBP... PNG is lossless), we use the same * base * values for all of them...
enum class IMAGE_QUALITY
{
	LOW = 10,
	MEDIUM = 25,
	HIGH = 50,								// default; may be tweaked dynamically depending on image encoding and client bandwidth
	HIGHER = 75,							// used for fullscreen updates
	HIGHEST = 100
};

struct wf_myrtille
{
	// pipes
	HANDLE inputsPipe;
	HANDLE updatesPipe;

	// inputs
	bool processInputs;

	// updates
	int imageEncoding;
	int imageQuality;
	int imageQuantity;
	int imageCount;
	int imageIdx;

	// display
	bool scaleDisplay;						// overrides the FreeRDP "SmartSizing" setting; the objective is not to interfere with the FreeRDP window, if shown
	int clientWidth;						// overrides wf_context::client_width (same purpose as above)
	int clientHeight;						// overrides wf_context::client_height

	// clipboard
	std::string clipboardText;
	bool clipboardUpdated;

	// GDI+
	ULONG_PTR gdiplusToken;
	CLSID pngClsid;
	CLSID jpgClsid;
	EncoderParameters encoderParameters;

	// WebP
	WebPConfig webpConfig;
};
typedef struct wf_myrtille wfMyrtille;

void wf_myrtille_start(wfContext* wfc)
{
	if (wfc->context.settings->MyrtilleSessionId == 0)
		return;

	wfc->myrtille = (wfMyrtille*)calloc(1, sizeof(wfMyrtille));
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	#if !defined(WITH_DEBUG) && !defined(_DEBUG)
	// by default, redirect stdout to nothing (same as linux "/dev/null")
	freopen("nul", "w", stdout);
	#endif

	// debug
	if (wfc->context.settings->MyrtilleDebugLog)
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

	/*
	prefixes (3 chars) are used to serialize commands with strings instead of numbers
	they make it easier to read log traces to find out which commands are issued
	they must match the prefixes used client side
	*/
	commandMap["RSZ"] = COMMAND::SEND_BROWSER_RESIZE;
	commandMap["KUC"] = COMMAND::SEND_KEY_UNICODE;
	commandMap["KSC"] = COMMAND::SEND_KEY_SCANCODE;
	commandMap["MMO"] = COMMAND::SEND_MOUSE_MOVE;
	commandMap["MLB"] = COMMAND::SEND_MOUSE_LEFT_BUTTON;
	commandMap["MMB"] = COMMAND::SEND_MOUSE_MIDDLE_BUTTON;
	commandMap["MRB"] = COMMAND::SEND_MOUSE_RIGHT_BUTTON;
	commandMap["MWU"] = COMMAND::SEND_MOUSE_WHEEL_UP;
	commandMap["MWD"] = COMMAND::SEND_MOUSE_WHEEL_DOWN;
	commandMap["STA"] = COMMAND::SET_STAT_MODE;
	commandMap["DBG"] = COMMAND::SET_DEBUG_MODE;
	commandMap["CMP"] = COMMAND::SET_COMPATIBILITY_MODE;
	commandMap["SCA"] = COMMAND::SET_SCALE_DISPLAY;
	commandMap["ECD"] = COMMAND::SET_IMAGE_ENCODING;
	commandMap["QLT"] = COMMAND::SET_IMAGE_QUALITY;
	commandMap["QNT"] = COMMAND::SET_IMAGE_QUANTITY;
	commandMap["FSU"] = COMMAND::REQUEST_FULLSCREEN_UPDATE;
	commandMap["CLP"] = COMMAND::REQUEST_REMOTE_CLIPBOARD;
	commandMap["CLO"] = COMMAND::CLOSE_RDP_CLIENT;

	// updates
	myrtille->imageEncoding = (int)IMAGE_ENCODING::AUTO;
	myrtille->imageQuality = (int)IMAGE_QUALITY::HIGH;
	myrtille->imageQuantity = IMAGE_COUNT_SAMPLING_RATE;
	myrtille->imageCount = 0;
	myrtille->imageIdx = 0;

	// display
	myrtille->scaleDisplay = false;
	myrtille->clientWidth = wfc->context.settings->DesktopWidth;
	myrtille->clientHeight = wfc->context.settings->DesktopHeight;

	// clipboard
	myrtille->clipboardText = "clipboard|";
	myrtille->clipboardUpdated = false;

	// GDI+
	GdiplusStartupInput gdiplusStartupInput;
	GdiplusStartup(&myrtille->gdiplusToken, &gdiplusStartupInput, NULL);

	GetEncoderClsid(L"image/png", &myrtille->pngClsid);
	GetEncoderClsid(L"image/jpeg", &myrtille->jpgClsid);

	int quality = (int)IMAGE_QUALITY::HIGH;
	EncoderParameters encoderParameters;
	encoderParameters.Count = 1;
	encoderParameters.Parameter[0].Guid = EncoderQuality;
	encoderParameters.Parameter[0].Type = EncoderParameterValueTypeLong;
	encoderParameters.Parameter[0].NumberOfValues = 1;
	encoderParameters.Parameter[0].Value = &quality;

	myrtille->encoderParameters = encoderParameters;

	// WebP
	float webpQuality = (int)IMAGE_QUALITY::HIGH;
	WebPConfig webpConfig;
	WebPConfigPreset(&webpConfig, WEBP_PRESET_PICTURE, webpQuality);

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

	myrtille->webpConfig = webpConfig;
}

void wf_myrtille_stop(wfContext* wfc)
{
	if (wfc->context.settings->MyrtilleSessionId == 0)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;
	myrtille->processInputs = false;
}

void wf_myrtille_connect(wfContext* wfc)
{
	if (wfc->context.settings->MyrtilleSessionId == 0)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// connect pipes
	DWORD result = connectRemoteSessionPipes(wfc);
	if (result != 0)
	{
		WLog_ERR(TAG, "wf_myrtille_connect: failed to connect session %i with error %d", wfc->context.settings->MyrtilleSessionId, result);
		return;
	}

	WLog_INFO(TAG, "wf_myrtille_connect: connected session %i", wfc->context.settings->MyrtilleSessionId);

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
	if (wfc->context.settings->MyrtilleSessionId == 0)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// --------------------------- consistency check ----------------------------------------------

	if (region.left < 0 || region.left > wfc->context.settings->DesktopWidth || region.top < 0 || region.top > wfc->context.settings->DesktopHeight ||
		region.right < 0 || region.right > wfc->context.settings->DesktopWidth || region.bottom < 0 || region.bottom > wfc->context.settings->DesktopHeight ||
		region.left > region.right || region.top > region.bottom)
		return;

	// --------------------------- ips regulator --------------------------------------------------

	if (myrtille->imageCount == INT_MAX)
	{
		myrtille->imageCount = 0;
	}

	myrtille->imageCount++;

	if (myrtille->imageQuantity == 5 ||
		myrtille->imageQuantity == 10 ||
		myrtille->imageQuantity == 20 ||
		myrtille->imageQuantity == 25 ||
		myrtille->imageQuantity == 50)
	{
		if (myrtille->imageCount % (100 / myrtille->imageQuantity) != 0)
			return;
	}

	// --------------------------- extract the consolidated region --------------------------------

	int cw, ch, dw, dh;
	cw = myrtille->clientWidth;
	ch = myrtille->clientHeight;
	dw = wfc->context.settings->DesktopWidth;
	dh = wfc->context.settings->DesktopHeight;

	HDC hdc = CreateCompatibleDC(wfc->primary->hdc);
	HBITMAP	hbmp;
		
	if (!myrtille->scaleDisplay || (cw == dw && ch == dh))
	{
		hbmp = CreateCompatibleBitmap(wfc->primary->hdc, region.right - region.left, region.bottom - region.top);
		SelectObject(hdc, hbmp);

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
	}
	else
	{
		hbmp = CreateCompatibleBitmap(wfc->primary->hdc, (region.right - region.left) * cw / dw, (region.bottom - region.top) * ch / dh);
		SelectObject(hdc, hbmp);

		SetStretchBltMode(hdc, HALFTONE);
		SetBrushOrgEx(hdc, 0, 0, NULL);
		StretchBlt(
			hdc,
			0,
			0,
			(region.right - region.left) * cw / dw,
			(region.bottom - region.top) * ch / dh,
			wfc->primary->hdc,
			region.left,
			region.top,
			region.right - region.left,
			region.bottom - region.top,
			SRCCOPY);

		// scale region
		region.left = region.left * cw / dw;
		region.top = region.top * ch / dh;
		region.right = region.right * cw / dw;
		region.bottom = region.bottom * ch / dh;
	}

	// debug, if needed
	//WLog_INFO(TAG, "wf_myrtille_send_region: left:%i, top:%i, right:%i, bottom:%i", region.left, region.top, region.right, region.bottom);

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
	if (wfc->context.settings->MyrtilleSessionId == 0)
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

	if (myrtille->imageIdx == INT_MAX)
	{
		myrtille->imageIdx = 0;
	}

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
			(int)IMAGE_FORMAT::CUR,
			(int)IMAGE_QUALITY::HIGHEST,
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
	if (wfc->context.settings->MyrtilleSessionId == 0)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;
	
	myrtille->clipboardText = "clipboard|";
	myrtille->clipboardUpdated = true;
}

void wf_myrtille_send_clipboard(wfContext* wfc, BYTE* data, UINT32 length)
{
	if (wfc->context.settings->MyrtilleSessionId == 0)
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
	ss << "\\\\.\\pipe\\remotesession_" << wfc->context.settings->MyrtilleSessionId << "_" << pipeName;
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
		ss << logDirectoryPath << "\\remotesession_" << wfc->context.settings->MyrtilleSessionId << "." << GetCurrentProcessId();
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
					COMMAND command = commandMap[inputs[i].substr(0, 3)];
					std::string commandArgs = inputs[i].substr(3, inputs[i].length() - 3);

					int separatorIdx;

					switch (command)
					{
						// browser resize
						case COMMAND::SEND_BROWSER_RESIZE:
							separatorIdx = commandArgs.find("x");
							if (separatorIdx != std::string::npos)
							{
								myrtille->clientWidth = stoi(commandArgs.substr(0, separatorIdx));
								myrtille->clientHeight = stoi(commandArgs.substr(separatorIdx + 1, commandArgs.length() - separatorIdx - 1));
							}
							break;

						// keystroke
						case COMMAND::SEND_KEY_UNICODE:
						case COMMAND::SEND_KEY_SCANCODE:
							separatorIdx = commandArgs.find("-");
							if (separatorIdx != std::string::npos)
							{
								std::string keyCode = commandArgs.substr(0, separatorIdx);
								std::string pressed = commandArgs.substr(separatorIdx + 1, 1);
								// character key
								if (command == COMMAND::SEND_KEY_UNICODE)
									wfc->context.input->UnicodeKeyboardEvent(wfc->context.input, (pressed == "1" ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE), atoi(keyCode.c_str()));
								// non character key
								else
									wfc->context.input->KeyboardEvent(wfc->context.input, (pressed == "1" ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE), atoi(keyCode.c_str()));
							}
							break;

						// mouse move
						case COMMAND::SEND_MOUSE_MOVE:
							ProcessMouseInput(wfc, commandArgs, PTR_FLAGS_MOVE);
							break;

						// mouse left button
						case COMMAND::SEND_MOUSE_LEFT_BUTTON:
							ProcessMouseInput(wfc, commandArgs.substr(1, commandArgs.length() - 1), commandArgs.substr(0, 1) == "0" ? PTR_FLAGS_BUTTON1 : PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON1);
							break;

						// mouse middle button
						case COMMAND::SEND_MOUSE_MIDDLE_BUTTON:
							ProcessMouseInput(wfc, commandArgs.substr(1, commandArgs.length() - 1), commandArgs.substr(0, 1) == "0" ? PTR_FLAGS_BUTTON3 : PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON3);
							break;

						// mouse right button
						case COMMAND::SEND_MOUSE_RIGHT_BUTTON:
							ProcessMouseInput(wfc, commandArgs.substr(1, commandArgs.length() - 1), commandArgs.substr(0, 1) == "0" ? PTR_FLAGS_BUTTON2 : PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON2);
							break;

						// mouse wheel up
						case COMMAND::SEND_MOUSE_WHEEL_UP:
							ProcessMouseInput(wfc, commandArgs, PTR_FLAGS_WHEEL | 0x0078);
							break;
						
						// mouse wheel down
						case COMMAND::SEND_MOUSE_WHEEL_DOWN:
							ProcessMouseInput(wfc, commandArgs, PTR_FLAGS_WHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x0088);
							break;

						// stat/debug/compatibility mode
						case COMMAND::SET_STAT_MODE:
						case COMMAND::SET_DEBUG_MODE:
						case COMMAND::SET_COMPATIBILITY_MODE:
							if (QueueUserWorkItem(SendReload, lpParameter, WT_EXECUTEDEFAULT) == 0)
								WLog_ERR(TAG, "ProcessInputsPipe: QueueUserWorkItem failed for SendReload with error %d", GetLastError());
							break;

						// scale display
						case COMMAND::SET_SCALE_DISPLAY:
							myrtille->scaleDisplay = commandArgs != "0";
							separatorIdx = commandArgs.find("x");
							if (separatorIdx != std::string::npos)
							{
								myrtille->clientWidth = stoi(commandArgs.substr(0, separatorIdx));
								myrtille->clientHeight = stoi(commandArgs.substr(separatorIdx + 1, commandArgs.length() - separatorIdx - 1));
							}
							if (QueueUserWorkItem(SendReload, lpParameter, WT_EXECUTEDEFAULT) == 0)
								WLog_ERR(TAG, "ProcessInputsPipe: QueueUserWorkItem failed for SendReload with error %d", GetLastError());
							break;

						// image encoding
						case COMMAND::SET_IMAGE_ENCODING:
							myrtille->imageEncoding = stoi(commandArgs);
							myrtille->imageQuality = (int)IMAGE_QUALITY::HIGH;
							break;

						// image quality is tweaked depending on the available client bandwidth (low available bandwidth = quality tweaked down)
						case COMMAND::SET_IMAGE_QUALITY:
							myrtille->imageQuality = stoi(commandArgs);
							break;

						// like for image quality, it's interesting to tweak down the image quantity if the available bandwidth gets too low
						// but skipping some images as well may also result in display inconsistencies, so be careful not to set it too low either (15 ips is a fair average in most cases)
						// to circumvent such inconsistencies, the combination with adaptive fullscreen update is nice because the whole screen is refreshed after a small user idle time (1,5 sec by default)
						case COMMAND::SET_IMAGE_QUANTITY:
							myrtille->imageQuantity = stoi(commandArgs);
							break;

						// fullscreen update
						case COMMAND::REQUEST_FULLSCREEN_UPDATE:
							if (QueueUserWorkItem(SendFullscreen, lpParameter, WT_EXECUTEDEFAULT) == 0)
								WLog_ERR(TAG, "ProcessInputsPipe: QueueUserWorkItem failed for SendFullscreen with error %d", GetLastError());
							break;

						// clipboard text
						case COMMAND::REQUEST_REMOTE_CLIPBOARD:
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

						// the standard way to close an rdp session is to logoff the user; an alternate way is to simply close the rdp client
						// this disconnect the session, which is then subsequently closed (1 sec later if "MaxDisconnectionTime" = 1000 ms)
						case COMMAND::CLOSE_RDP_CLIENT:
							myrtille->processInputs = false;
							break;
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

void ProcessMouseInput(wfContext* wfc, std::string input, UINT16 flags)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	int separatorIdx = input.find("-");
	if (separatorIdx != std::string::npos)
	{
		std::string mX = input.substr(0, separatorIdx);
		std::string mY = input.substr(separatorIdx + 1, input.length() - separatorIdx - 1);
		if (!mX.empty() && stoi(mX) >= 0 && !mY.empty() && stoi(mY) >= 0)
		{
			int cw, ch, dw, dh;
			cw = myrtille->clientWidth;
			ch = myrtille->clientHeight;
			dw = wfc->context.settings->DesktopWidth;
			dh = wfc->context.settings->DesktopHeight;

			if (!myrtille->scaleDisplay || (cw == dw && ch == dh))
			{
				wfc->context.input->MouseEvent(
					wfc->context.input,
					flags,
					stoi(mX),
					stoi(mY));
			}
			else
			{
				wfc->context.input->MouseEvent(
					wfc->context.input,
					flags,
					stoi(mX) * dw / cw,
					stoi(mY) * dh / ch);
			}
		}
	}
}

DWORD WINAPI SendFullscreen(LPVOID lpParameter)
{
	wfContext* wfc = (wfContext*)lpParameter;
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// --------------------------- pipe check -----------------------------------------------------

	if (wfc->context.settings->MyrtilleSessionId == 0)
		return 0;

	// --------------------------- retrieve the fullscreen bitmap ---------------------------------

	int cw, ch, dw, dh;
	cw = myrtille->clientWidth;
	ch = myrtille->clientHeight;
	dw = wfc->context.settings->DesktopWidth;
	dh = wfc->context.settings->DesktopHeight;

	HDC hdc = CreateCompatibleDC(wfc->primary->hdc);
	HBITMAP hbmp = CreateCompatibleBitmap(wfc->primary->hdc, myrtille->scaleDisplay ? cw : dw, myrtille->scaleDisplay ? ch : dh);
	SelectObject(hdc, hbmp);

	if (!myrtille->scaleDisplay || (cw == dw && ch == dh))
	{
		BitBlt(hdc,	0, 0, dw, dh, wfc->primary->hdc, 0,	0, SRCCOPY);
	}
	else
	{
		SetStretchBltMode(hdc, HALFTONE);
		SetBrushOrgEx(hdc, 0, 0, NULL);
		StretchBlt(hdc,	0, 0, cw, ch, wfc->primary->hdc, 0,	0, dw, dh, SRCCOPY);
	}

	// debug, if needed
	//WLog_INFO(TAG, "SendFullscreen");

	Gdiplus::Bitmap *bmpScreen = Gdiplus::Bitmap::FromHBITMAP(hbmp, (HPALETTE)0);

	// ---------------------------  process it ----------------------------------------------------

	processImage(wfc, bmpScreen, 0, 0, myrtille->scaleDisplay ? cw : dw, myrtille->scaleDisplay ? ch : dh, true);

	// ---------------------------  cleanup -------------------------------------------------------

	delete bmpScreen;
	bmpScreen = NULL;

	DeleteObject(hbmp);
	hbmp = NULL;

	DeleteDC(hdc);
	hdc = NULL;

	return 0;
}

DWORD WINAPI SendReload(LPVOID lpParameter)
{
	wfContext* wfc = (wfContext*)lpParameter;
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// --------------------------- pipe check -----------------------------------------------------

	if (wfc->context.settings->MyrtilleSessionId == 0)
		return 0;

	// --------------------------- reload request -------------------------------------------------

	std::stringstream ss;
	ss << "reload";
	std::string s = ss.str();

	DWORD bytesWritten;
	if (WriteFile(myrtille->updatesPipe, s.c_str(), s.length(), &bytesWritten, NULL) == 0)
	{
		WLog_ERR(TAG, "ProcessInputsPipe: WriteFile failed for reload with error %d", GetLastError());
	}

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
	int quality = (fullscreen ? (int)IMAGE_QUALITY::HIGHER : myrtille->imageQuality);	// use higher quality for fullscreen updates; otherwise current
	IStream* stream = NULL;
	ULONG size = 0;

	/*
	normally, the PNG format is best suited (lower size and better quality) for office applications (with text) and JPG for graphic ones (with images)
	PNG is lossless as opposite to JPG
	WEBP can either be lossy or lossless
	*/

	if (myrtille->imageEncoding == (int)IMAGE_ENCODING::PNG || myrtille->imageEncoding == (int)IMAGE_ENCODING::JPEG || myrtille->imageEncoding == (int)IMAGE_ENCODING::AUTO)
	{
		ULONG pngSize;
		ULONG jpgSize;

		// --------------------------- convert the bitmap into PNG --------------------------------

		if (myrtille->imageEncoding == (int)IMAGE_ENCODING::PNG || myrtille->imageEncoding == (int)IMAGE_ENCODING::AUTO)
		{
			CreateStreamOnHGlobal(NULL, TRUE, &pngStream);
			bmp->Save(pngStream, &myrtille->pngClsid);

			pngStream->Stat(&statstg, STATFLAG_DEFAULT);
			pngSize = (ULONG)statstg.cbSize.LowPart;
		}

		// --------------------------- convert the bitmap into JPEG -------------------------------

		if (myrtille->imageEncoding == (int)IMAGE_ENCODING::JPEG || myrtille->imageEncoding == (int)IMAGE_ENCODING::AUTO)
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

		if (myrtille->imageEncoding == (int)IMAGE_ENCODING::PNG || (myrtille->imageEncoding == (int)IMAGE_ENCODING::AUTO && pngSize <= jpgSize))
		{
			stream = pngStream;
			format = (int)IMAGE_FORMAT::PNG;
			quality = (int)IMAGE_QUALITY::HIGHEST;	// lossless
			size = pngSize;
		}
		else
		{
			stream = jpgStream;
			format = (int)IMAGE_FORMAT::JPEG;
			size = jpgSize;
		}
	}
	else if (myrtille->imageEncoding == (int)IMAGE_ENCODING::WEBP)
	{
		// --------------------------- convert the bitmap into WEBP -------------------------------

		CreateStreamOnHGlobal(NULL, TRUE, &webpStream);
		WebPEncoder(wfc, bmp, myrtille->imageIdx + 1, webpStream, quality, fullscreen);

		webpStream->Stat(&statstg, STATFLAG_DEFAULT);
		ULONG webpSize = (ULONG)statstg.cbSize.LowPart;

		stream = webpStream;
		format = (int)IMAGE_FORMAT::WEBP;
		size = webpSize;
	}

	// ---------------------------  send the image ------------------------------------------------

	if (myrtille->imageIdx == INT_MAX)
	{
		myrtille->imageIdx = 0;
	}

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
			case (int)IMAGE_FORMAT::CUR:
				ss << "\\cursor_" << idx << ".png";
				break;

			case (int)IMAGE_FORMAT::PNG:
				ss << (fullscreen ? "\\screen_" : "\\region_") << idx << ".png";
				break;

			case (int)IMAGE_FORMAT::JPEG:
				ss << (fullscreen ? "\\screen_" : "\\region_") << idx << "_" << quality << ".jpg";
				break;
		}

		std::string s = ss.str();
		std::wstring ws = s2ws(s);
		const wchar_t *filename = ws.c_str();

		switch (format)
		{
			case (int)IMAGE_FORMAT::CUR:
			case (int)IMAGE_FORMAT::PNG:
				bmp->Save(filename, &myrtille->pngClsid);
				break;

			case (int)IMAGE_FORMAT::JPEG:
				myrtille->encoderParameters.Parameter[0].Value = &quality;
				bmp->Save(filename, &myrtille->jpgClsid, &myrtille->encoderParameters);
				break;
		}
	}
}

void sendImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, int posX, int posY, int width, int height, int format, int quality, IStream* stream, int size, bool fullscreen)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// image structure: tag (4 bytes) + info (32 bytes) + data
	// > tag is used to identify an image (0: image; text message otherwise)
	// > info contains the image metadata (idx, posX, posY, etc.)
	// > data is the image raw data

	// tag + info
	byte* info = new byte[36];
	int32ToBytes(0, 0, info);
	int32ToBytes(idx, 4, info);
	int32ToBytes(posX, 8, info);
	int32ToBytes(posY, 12, info);
	int32ToBytes(width, 16, info);
	int32ToBytes(height, 20, info);
	int32ToBytes(format, 24, info);
	int32ToBytes(quality, 28, info);
	int32ToBytes(fullscreen, 32, info);

	// seek to the beginning of the stream
	LARGE_INTEGER li = { 0 };
	stream->Seek(li, STREAM_SEEK_SET, NULL);

	// data
	ULONG bytesRead;
	byte* data = new byte[size];
	stream->Read(data, size, &bytesRead);

	// tag + info + data
	byte* buf = new byte[size + 36];
	memcpy(buf, info, 36);
	memcpy(&buf[36], data, size);

	DWORD bytesToWrite = size + 36;
	DWORD bytesWritten;

	// enqueue it
	if (WriteFile(myrtille->updatesPipe, buf, bytesToWrite, &bytesWritten, NULL) == 0)
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
	delete[] buf;
	buf = NULL;

	delete[] data;
	data = NULL;

	delete[] info;
	info = NULL;
}

void int32ToBytes(int value, int startIndex, byte* bytes)
{
	// little endian
	bytes[startIndex] = value & 0xFF;
	bytes[startIndex + 1] = (value >> 8) & 0xFF;
	bytes[startIndex + 2] = (value >> 16) & 0xFF;
	bytes[startIndex + 3] = (value >> 24) & 0xFF;
}

void WebPEncoder(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, IStream* stream, float quality, bool fullscreen)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	WebPPicture webpPic;

	if (WebPPictureInit(&webpPic))
	{
		webpPic.user_data = NULL;

		// debug

		//std::string imgDirectoryPath = createRemoteSessionDirectory(wfc);
		//if (imgDirectoryPath != "")
		//{
		//	std::stringstream ss;
		//	ss << imgDirectoryPath << (fullscreen ? "\\screen_" : "\\region_") << idx << "_" << quality << ".webp";
		//	webpPic.user_data = new std::string(ss.str());
		//}

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

		if (webpPic.user_data != NULL)
			delete webpPic.user_data;

		WebPPictureFree(&webpPic);
	}
}

static int WebPWriter(const uint8_t* data, size_t data_size, const WebPPicture* const pic)
{
	IStream* stream = (IStream*)pic->custom_ptr;

	ULONG bytesWritten;
	stream->Write(data, data_size, &bytesWritten);

	// debug

	//if (pic->user_data != NULL)
	//{
	//	std::string &filename = *(std::string*)pic->user_data;
	//	FILE* file = fopen(filename.c_str(), "ab");
	//	if (file != NULL)
	//	{
	//		fwrite(data, 1, data_size, file);
	//		fclose(file);
	//	}
	//}

	return bytesWritten == data_size ? 1 : 0;
}

#pragma endregion
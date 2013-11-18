//  ClipWatcher.cpp
//

#include <stdio.h>
#include <stdlib.h>
#include <Windows.h>
#include <StrSafe.h>
#include <Shlobj.h>
#include <dbt.h>
#include "Resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

const LPCWSTR DEFAULT_CLIPPATH = L"Clipboard";
const LPCWSTR WINDOW_TITLE = L"ClipWatcher";
const LPCWSTR WATCHING_DIR = L"Watching: %s";
const LPCWSTR CLIPBOARD_UPDATED = L"Clipboard Updated";
const LPCWSTR CLIPBOARD_TYPE_BITMAP = L"Bitmap";

const LPCWSTR OP_OPEN = L"open";
const DWORD MAX_FILE_SIZE = 32767;
const int CLIPBOARD_RETRY = 3;
const UINT CLIPBOARD_DELAY = 100;
const UINT WM_NOTIFY_ICON = WM_USER+1;
const UINT WM_NOTIFY_FILE = WM_NOTIFY_ICON+1;
const UINT TIMER_INTERVAL = 400;

static FILE* logfp = stderr;

typedef enum _MenuItemID {
    IDM_NONE = 0,
    IDM_EXIT,
    IDM_OPEN,
};

static int getNumColors(BITMAPINFO* bmp)
{
    int ncolors = bmp->bmiHeader.biClrUsed;
    if (ncolors == 0) {
        switch (bmp->bmiHeader.biBitCount) {
        case 1:
            ncolors = 2;
        case 8:
            ncolors = 256;
        }
    }
    return ncolors;
}

static size_t getBMPHeaderSize(BITMAPINFO* bmp)
{
    return (bmp->bmiHeader.biSize + getNumColors(bmp)*sizeof(RGBQUAD));
}

static size_t getBMPSize(BITMAPINFO* bmp)
{
    return (getBMPHeaderSize(bmp) + bmp->bmiHeader.biSizeImage);
}

static LPWSTR getWCHARfromCHAR(LPCSTR bytes, int nbytes, int* pnchars)
{
    int nchars = MultiByteToWideChar(CP_ACP, 0, bytes, nbytes, NULL, 0);
    LPWSTR chars = (LPWSTR) malloc(sizeof(WCHAR)*(nchars+1));
    if (chars != NULL) {
	MultiByteToWideChar(CP_ACP, 0, bytes, nbytes, chars, nchars);
	chars[nchars] = L'\0';
	if (pnchars != NULL) {
	    *pnchars = nchars;
	}
    }
    return chars;
}

static LPSTR getCHARfromWCHAR(LPCWSTR chars, int nchars, int* pnbytes)
{
    int nbytes = WideCharToMultiByte(CP_ACP, 0, chars, nchars, 
				     NULL, 0, NULL, NULL);
    LPSTR bytes = (LPSTR) malloc(sizeof(CHAR)*(nbytes+1));
    if (bytes != NULL) {
	WideCharToMultiByte(CP_ACP, 0, chars, nchars, 
			    (LPSTR)bytes, nbytes, NULL, NULL);
	bytes[nbytes] = 0;
	if (pnbytes != NULL) {
	    *pnbytes = nbytes;
	}
    }
    return bytes;
}

// ristrip(text1, text2)
static LPWSTR ristrip(LPCWSTR text1, LPCWSTR text2)
{
    int len1 = wcslen(text1);
    int len2 = wcslen(text2);
    int dstlen = len1;
    if (len2 < len1 && wcsicmp(&text1[len1-len2], text2) == 0) {
	dstlen -= len2;
    }

    LPWSTR dst = (LPWSTR) malloc(sizeof(WCHAR)*(dstlen+1));
    if (dst != NULL) {
	StringCchCopy(dst, dstlen+1, text1);
    }
    return dst;
}

// rindex(filename)
static int rindex(LPCWSTR text, WCHAR c)
{
    int i = wcslen(text)-1;
    while (0 <= i) {
        if (text[i] == c) return i;
        i--;
    }
    return -1;
}

// istartswith(text1, text2)
static int istartswith(LPCWSTR text1, LPCWSTR text2)
{
    while (*text1 != 0 && *text2 != 0 && *text1 == *text2) {
	text1++;
	text2++;
    }
    return (*text2 == 0);
}

// setClipboardText(text, nchars)
static void setClipboardText(HWND hWnd, LPCWSTR text, int nchars)
{
    int nbytes;
    LPSTR src = getCHARfromWCHAR(text, nchars, &nbytes);
    if (src != NULL) {
	HANDLE data = GlobalAlloc(GHND, nbytes+1);
	if (data != NULL) {
	    LPSTR dst = (LPSTR) GlobalLock(data);
	    if (dst != NULL) {
		CopyMemory(dst, src, nbytes+1);
		GlobalUnlock(data);
                for (int i = 0; i < CLIPBOARD_RETRY; i++) {
                    Sleep(CLIPBOARD_DELAY);
                    if (OpenClipboard(hWnd)) {
                        SetClipboardData(CF_TEXT, data);
                        CloseClipboard();
                        break;
                    }
                }
                GlobalFree(data);
	    }
	}
	free(src);
    }
}

// writeClipBytes(path, bytes, nbytes)
static void writeClipBytes(LPCWSTR path, LPVOID bytes, int nbytes)
{
    HANDLE fp = CreateFile(path, GENERIC_WRITE, 0,
			   NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 
			   NULL);
    if (fp != INVALID_HANDLE_VALUE) {
	fwprintf(logfp, L"write file: path=%s, nbytes=%u\n", path, nbytes);
        DWORD writtenbytes;
        WriteFile(fp, bytes, nbytes, &writtenbytes, NULL);
	CloseHandle(fp);
    }
}

// writeClipText(path, text, nchars)
static void writeClipText(LPCWSTR path, LPCWSTR text, int nchars)
{
    int nbytes;
    LPSTR bytes = getCHARfromWCHAR(text, nchars, &nbytes);
    nbytes--;
    if (bytes != NULL) {
        writeClipBytes(path, bytes, nbytes);
        free(bytes);
    }
}

// writeClipDIB(path, bytes, nbytes)
static void writeClipDIB(LPCWSTR path, LPVOID bytes, SIZE_T nbytes)
{
    BITMAPFILEHEADER filehdr = {0};
    filehdr.bfType = 0x4d42;    // 'BM' in little endian.
    filehdr.bfSize = sizeof(filehdr)+nbytes;
    filehdr.bfOffBits = sizeof(filehdr)+getBMPHeaderSize((BITMAPINFO*)bytes);
    HANDLE fp = CreateFile(path, GENERIC_WRITE, 0,
			   NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 
			   NULL);
    if (fp != INVALID_HANDLE_VALUE) {
	fwprintf(logfp, L"write file: path=%s, nbytes=%u\n", path, filehdr.bfSize);
        DWORD writtenbytes;
        WriteFile(fp, &filehdr, sizeof(filehdr), &writtenbytes, NULL);
        WriteFile(fp, bytes, nbytes, &writtenbytes, NULL);
	CloseHandle(fp);
    }
}

// readClipText(path, &nchars)
static LPWSTR readClipText(LPCWSTR path, int* nchars)
{
    LPWSTR text = NULL;
    HANDLE fp = CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
			   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 
			   NULL);
    if (fp != INVALID_HANDLE_VALUE) {
	DWORD nbytes = GetFileSize(fp, NULL);
	if (MAX_FILE_SIZE < nbytes) {
	    nbytes = MAX_FILE_SIZE;
	}
	fwprintf(logfp, L"read file: path=%s, nbytes=%u\n", path, nbytes);
	LPSTR bytes = (LPSTR) malloc(nbytes);
	if (bytes != NULL) {
	    DWORD readbytes;
	    ReadFile(fp, bytes, nbytes, &readbytes, NULL);
	    text = getWCHARfromCHAR(bytes, (int)readbytes, nchars);
	    free(bytes);
	}
	CloseHandle(fp);
    }

    return text;
}

// openClipText(name, text, nchars)
static void openClipText(LPCWSTR name, LPCWSTR text, int nchars)
{
    if (istartswith(text, L"http://") ||
	istartswith(text, L"https://")) {
	LPWSTR url = (LPWSTR) malloc(sizeof(WCHAR)*(nchars+1));
	if (url != NULL) {
	    WCHAR* p = url;
	    for (int i = 0; i < nchars; i++) {
		WCHAR c = text[i];
		if (L' ' < c) {
		    *(p++) = c;
		}
	    }
	    *p = L'\0';
	    ShellExecute(NULL, OP_OPEN, url, NULL, NULL, SW_SHOWDEFAULT);
	    free(url);
	}
    } else {
	WCHAR temppath[MAX_PATH];
	GetTempPath(MAX_PATH, temppath);
	WCHAR path[MAX_PATH];
	StringCchPrintf(path, _countof(path), L"%s\\%s.txt", temppath, name);
	writeClipText(path, text, nchars);
	ShellExecute(NULL, OP_OPEN, path, NULL, NULL, SW_SHOWDEFAULT);
    }
}

// getFileSignature(fp)
static DWORD getFileSignature(HANDLE fp, DWORD n)
{
    DWORD sig = 0;
    DWORD bufsize = sizeof(DWORD)*n;
    DWORD* buf = (DWORD*) malloc(bufsize);
    if (buf != NULL) {
	DWORD readbytes;
	ZeroMemory(buf, bufsize);
	ReadFile(fp, buf, bufsize, &readbytes, NULL);
	for (int i = 0; i < n; i++) {
	    sig ^= buf[i];
	}
	free(buf);
    }
    return sig;
}


//  FileEntry
// 
typedef struct _FileEntry {
    WCHAR name[MAX_PATH];
    DWORD sig;
    struct _FileEntry* next;
} FileEntry;

//  ClipWatcher
// 
typedef struct _ClipWatcher {
    LPWSTR dstdir;
    LPWSTR srcdir;
    HANDLE notifier;
    LPWSTR name;
    FileEntry* files;
    DWORD seqno;

    HICON icons[2];
    UINT icon_id;
    UINT_PTR timer_id;
    int blink_count;
} ClipWatcher;

// findFileEntry(files, name)
static FileEntry* findFileEntry(FileEntry* entry, LPCWSTR name)
{
    while (entry != NULL) {
	if (wcsicmp(entry->name, name) == 0) return entry;
	entry = entry->next;
    }
    return entry;
}

// freeFileEntries(files)
static void freeFileEntries(FileEntry* entry)
{
    while (entry != NULL) {
	void* p = entry;
	entry = entry->next;
	free(p);
    }
}

// checkFileChanges(watcher)
static FileEntry* checkFileChanges(ClipWatcher* watcher)
{
    WCHAR dirpath[MAX_PATH];
    StringCchPrintf(dirpath, _countof(dirpath), L"%s\\*.*", watcher->srcdir);

    WIN32_FIND_DATA data;
    FileEntry* found = NULL;

    HANDLE fft = FindFirstFile(dirpath, &data);
    if (fft == NULL) goto fail;
    
    for (;;) {
        LPWSTR name = data.cFileName;
        int index = rindex(name, L'.');
        if (0 <= index && wcsncmp(name, watcher->name, index) != 0) {
            WCHAR path[MAX_PATH];
            StringCchPrintf(path, _countof(path), L"%s\\%s", 
                            watcher->srcdir, name);
            HANDLE fp = CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, 
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING,
                                   NULL);
            if (fp != INVALID_HANDLE_VALUE) {
                DWORD sig = getFileSignature(fp, 256);
                fwprintf(logfp, L"sig: %s: %08x\n", name, sig);
                FileEntry* entry = findFileEntry(watcher->files, name);
                if (entry == NULL) {
                    fwprintf(logfp, L"added: %s\n", name);
                    entry = (FileEntry*) malloc(sizeof(FileEntry));
                    StringCchCopy(entry->name, _countof(entry->name), name);
                    entry->sig = sig;
                    entry->next = watcher->files;
                    watcher->files = entry;
                    found = entry;
                } else if (sig != entry->sig) {
                    fwprintf(logfp, L"updated: %s\n", name);
                    entry->sig = sig;
                    found = entry;
                }
                CloseHandle(fp);
            }
        }
	if (!FindNextFile(fft, &data)) break;
    }
    FindClose(fft);

fail:
    return found;
}

//  CreateClipWatcher
// 
ClipWatcher* CreateClipWatcher(
    HINSTANCE hInstance, 
    LPCWSTR dstdir, LPCWSTR srcdir, LPCWSTR name)
{
    ClipWatcher* watcher = (ClipWatcher*) malloc(sizeof(ClipWatcher));
    if (watcher == NULL) return NULL;

    watcher->dstdir = wcsdup(dstdir);
    watcher->srcdir = wcsdup(srcdir);
    watcher->notifier = INVALID_HANDLE_VALUE;
    watcher->name = wcsdup(name);
    watcher->files = NULL;
    watcher->seqno = 0;

    watcher->icons[0] = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CLIPEMPTY));
    watcher->icons[1] = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CLIPFULL));
    watcher->icon_id = 1;
    watcher->timer_id = 1;
    watcher->blink_count = 0;
    return watcher;
}

//  UpdateClipWatcher
// 
void UpdateClipWatcher(ClipWatcher* watcher)
{
    if (watcher->notifier != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(watcher->notifier);
    }
    // Register a file watcher.
    watcher->notifier = FindFirstChangeNotification(
	watcher->srcdir, FALSE, 
	(FILE_NOTIFY_CHANGE_FILE_NAME |
	 FILE_NOTIFY_CHANGE_SIZE |
	 FILE_NOTIFY_CHANGE_ATTRIBUTES |
	 FILE_NOTIFY_CHANGE_LAST_WRITE));
    fwprintf(logfp, L"register: srcdir=%s, notifier=%p\n", 
             watcher->srcdir, watcher->notifier);
}

//  DestroyClipWatcher
// 
void DestroyClipWatcher(ClipWatcher* watcher)
{
    if (watcher->notifier != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(watcher->notifier);
    }
    if (watcher->srcdir != NULL) {
	free(watcher->srcdir);
    }
    if (watcher->dstdir != NULL) {
	free(watcher->dstdir);
    }
    if (watcher->name != NULL) {
	free(watcher->name);
    }

    freeFileEntries(watcher->files);

    free(watcher);
}

// popupInfo
static void popupInfo(HWND hWnd, UINT icon_id, LPCWSTR title, LPCWSTR text)
{
    NOTIFYICONDATA nidata = {0};
    nidata.cbSize = sizeof(nidata);
    nidata.hWnd = hWnd;
    nidata.uID = icon_id;
    nidata.uFlags = NIF_INFO;
    nidata.dwInfoFlags = NIIF_INFO;
    nidata.uTimeout = 1;
    StringCchCopy(nidata.szInfoTitle, _countof(nidata.szInfoTitle), title);
    StringCchCopy(nidata.szInfo, _countof(nidata.szInfo), text);
    Shell_NotifyIcon(NIM_MODIFY, &nidata);
}

// showIcon
static void showIcon(HWND hWnd, UINT icon_id, HICON hIcon)
{
    NOTIFYICONDATA nidata = {0};
    nidata.cbSize = sizeof(nidata);
    nidata.hWnd = hWnd;
    nidata.uID = icon_id;
    nidata.uFlags = NIF_ICON;
    nidata.hIcon = hIcon;
    Shell_NotifyIcon(NIM_MODIFY, &nidata);
}

// displayContextMenu
static void displayContextMenu(HWND hWnd, POINT pt)
{
    HMENU menu = CreatePopupMenu();
    if (menu != NULL) {
	AppendMenu(menu, MF_STRING | MF_ENABLED,
		   IDM_OPEN, L"Open");
	AppendMenu(menu, MF_STRING | MF_ENABLED,
		   IDM_EXIT, L"Exit");
	TrackPopupMenu(menu, TPM_LEFTALIGN, 
		       pt.x, pt.y, 0, hWnd, NULL);
	DestroyMenu(menu);
    }
}


//  clipWatcherWndProc
//
static LRESULT CALLBACK clipWatcherWndProc(
    HWND hWnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    //fwprintf(stderr, L"msg: %x, hWnd=%p, wParam=%p\n", uMsg, hWnd, wParam);

    switch (uMsg) {
    case WM_CREATE:
    {
        // Initialization.
	CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
	ClipWatcher* watcher = (ClipWatcher*)(cs->lpCreateParams);
	if (watcher != NULL) {
	    SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)watcher);
	    fwprintf(logfp, L"watcher: %s\n", watcher->name);
	    // Start watching the clipboard content.
            AddClipboardFormatListener(hWnd);
	    // Register the icon.
	    NOTIFYICONDATA nidata = {0};
	    nidata.cbSize = sizeof(nidata);
	    nidata.hWnd = hWnd;
	    nidata.uID = watcher->icon_id;
	    nidata.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	    nidata.uCallbackMessage = WM_NOTIFY_ICON;
	    nidata.hIcon = watcher->icons[0];
	    StringCchPrintf(nidata.szTip, _countof(nidata.szTip),
			    WATCHING_DIR, watcher->srcdir);
	    Shell_NotifyIcon(NIM_ADD, &nidata);
            SetTimer(hWnd, watcher->timer_id, TIMER_INTERVAL, NULL);
	}
	return FALSE;
    }
    
    case WM_DESTROY:
    {
        // Clean up.
	LONG_PTR lp = GetWindowLongPtr(hWnd, GWLP_USERDATA);
	ClipWatcher* watcher = (ClipWatcher*)lp;
	if (watcher != NULL) {
            KillTimer(hWnd, watcher->timer_id);
	    // Stop watching the clipboard content.
            RemoveClipboardFormatListener(hWnd);
	    // Unregister the icon.
	    NOTIFYICONDATA nidata = {0};
	    nidata.cbSize = sizeof(nidata);
	    nidata.hWnd = hWnd;
	    nidata.uID = watcher->icon_id;
	    Shell_NotifyIcon(NIM_DELETE, &nidata);
	}
	PostQuitMessage(0);
	return FALSE;
    }

    case WM_CLIPBOARDUPDATE:
    {
        // Clipboard change detected.
	LONG_PTR lp = GetWindowLongPtr(hWnd, GWLP_USERDATA);
	ClipWatcher* watcher = (ClipWatcher*)lp;
	if (watcher != NULL) {
            DWORD seqno = GetClipboardSequenceNumber();
	    if (watcher->seqno < seqno) {
                watcher->seqno = seqno;
                fwprintf(logfp, L"clipboard changed: seqno=%d\n", seqno);
                for (int i = 0; i < CLIPBOARD_RETRY; i++) {
                    Sleep(CLIPBOARD_DELAY);
                    if (OpenClipboard(hWnd)) {
                        // CF_TEXT
                        HANDLE data = GetClipboardData(CF_TEXT);
                        if (data != NULL) {
                            LPSTR bytes = (LPSTR) GlobalLock(data);
                            if (bytes != NULL) {
                                int nchars;
                                LPWSTR text = getWCHARfromCHAR(bytes, GlobalSize(data), &nchars);
                                if (text != NULL) {
                                    WCHAR path[MAX_PATH];
                                    StringCchPrintf(path, _countof(path), L"%s\\%s.txt", 
                                                    watcher->dstdir, watcher->name);
                                    writeClipText(path, text, nchars);
                                    popupInfo(hWnd, watcher->icon_id,
                                              CLIPBOARD_UPDATED, text);
                                    watcher->blink_count = 10;
                                    free(text);
                                }
                                GlobalUnlock(data);
                            }
                        }
                        // CF_DIB
                        data = GetClipboardData(CF_DIB);
                        if (data != NULL) {
                            LPVOID bytes = GlobalLock(data);
                            if (bytes != NULL) {
                                SIZE_T nbytes = GlobalSize(data);
                                WCHAR path[MAX_PATH];
                                StringCchPrintf(path, _countof(path), L"%s\\%s.bmp", 
                                                watcher->dstdir, watcher->name);
                                writeClipDIB(path, bytes, nbytes);
                                popupInfo(hWnd, watcher->icon_id,
                                          CLIPBOARD_UPDATED, CLIPBOARD_TYPE_BITMAP);
                                GlobalUnlock(bytes);
                            }
                        }
                        CloseClipboard();
                        break;
                    }
                }
	    }
	}
	return FALSE;
    }

    case WM_NOTIFY_FILE:
    {
        // File change detected.
	LONG_PTR lp = GetWindowLongPtr(hWnd, GWLP_USERDATA);
	ClipWatcher* watcher = (ClipWatcher*)lp;
	if (watcher != NULL) {
	    FileEntry* entry = checkFileChanges(watcher);
	    if (entry != NULL) {
		fwprintf(logfp, L"file changed: %s\n", entry->name);
                int index = rindex(entry->name, L'.');
                if (0 <= index && wcsncmp(entry->name, watcher->name, index) != 0) {
                    WCHAR* ext = &(entry->name[index]);
                    WCHAR path[MAX_PATH];
                    StringCchPrintf(path, _countof(path), L"%s\\%s", 
                                    watcher->srcdir, entry->name);
                    if (_wcsicmp(ext, L".txt") == 0) {
                        // CF_TEXT
                        int nchars;
                        LPWSTR text = readClipText(path, &nchars);
                        if (text != NULL) {
                            setClipboardText(hWnd, text, nchars);
                            free(text);
                        }
                    } else if (_wcsicmp(ext, L".bmp") == 0) {
                        // CF_DIB
#if 0
                        BITMAPINFO* bmp = readClipDIB(path);
                        if (bmp != NULL) {
                            setClipboardDIB(hWnd, bmp);
                            free(bmp);
                        }
#endif
                    }
		}
	    }
	}
	return FALSE;
    }

    case WM_COMMAND:
    {
        // Command specified.
	LONG_PTR lp = GetWindowLongPtr(hWnd, GWLP_USERDATA);
	ClipWatcher* watcher = (ClipWatcher*)lp;
	switch (LOWORD(wParam)) {
	case IDM_OPEN:
	    if (watcher != NULL) {
		if (OpenClipboard(hWnd)) {
		    HANDLE data = GetClipboardData(CF_TEXT);
		    if (data != NULL) {
			LPSTR bytes = (LPSTR) GlobalLock(data);
			if (bytes != NULL) {
			    int nchars;
			    LPWSTR text = getWCHARfromCHAR(bytes, GlobalSize(data), &nchars);
			    if (text != NULL) {
				openClipText(watcher->name, text, nchars);
				free(text);
			    }
			    GlobalUnlock(data);
			}
		    }
		    CloseClipboard();
		}
	    }
	    break;
	case IDM_EXIT:
	    SendMessage(hWnd, WM_CLOSE, 0, 0);
	    break;
	}
	return FALSE;
    }

    case WM_TIMECHANGE:
    {
        // Filesytem/Network share change detected.
        // NOTICE: We wanted to check if wParam is DBT_DEVICEARRIVAL.
        //   But it doesn't work when the system is suspended.
	LONG_PTR lp = GetWindowLongPtr(hWnd, GWLP_USERDATA);
	ClipWatcher* watcher = (ClipWatcher*)lp;
	if (watcher != NULL) {
            // Re-initialize the watcher object.
            UpdateClipWatcher(watcher);
        }
        return TRUE;
    }

    case WM_NOTIFY_ICON:
    {
        // UI event handling.
	POINT pt;
	switch (lParam) {
	case WM_LBUTTONDBLCLK:
	    SendMessage(hWnd, WM_COMMAND, 
			MAKEWPARAM(IDM_OPEN, 1), NULL);
	    break;
	case WM_LBUTTONUP:
	    break;
	case WM_RBUTTONUP:
	    if (GetCursorPos(&pt)) {
		SetForegroundWindow(hWnd);
		displayContextMenu(hWnd, pt);
		PostMessage(hWnd, WM_NULL, 0, 0);
	    }
	    break;
	}
	return FALSE;
    }

    case WM_TIMER:
    {
	LONG_PTR lp = GetWindowLongPtr(hWnd, GWLP_USERDATA);
	ClipWatcher* watcher = (ClipWatcher*)lp;
	if (watcher != NULL) {
            // Blink the icon.
            if (watcher->blink_count) {
                watcher->blink_count--;
                showIcon(hWnd, watcher->icon_id,
                         watcher->icons[(watcher->blink_count % 2)]);
            }
        }
        return FALSE;
    }

    case WM_CLOSE:
	DestroyWindow(hWnd);
	return FALSE;

    default:
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
}


//  ClipWatcherMain
// 
int ClipWatcherMain(
    HINSTANCE hInstance, 
    HINSTANCE hPrevInstance, 
    int nCmdShow,
    int argc, LPWSTR* argv)
{
    LPCWSTR clippath = DEFAULT_CLIPPATH;
    if (2 <= argc) {
	clippath = argv[1];
    }

    // Prevent a duplicate process.
    HANDLE mutex = CreateMutex(NULL, TRUE, L"ClipWatcher");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
	CloseHandle(mutex);
	return 0;
    }
    
    // Obtain the clipboard directory path.
    WCHAR clipdir[MAX_PATH];
    if (GetFileAttributes(clippath) == INVALID_FILE_ATTRIBUTES) {
	// Obtain the home path.
	WCHAR home[MAX_PATH];
	SHGetFolderPath(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, home);
	StringCchPrintf(clipdir, _countof(clipdir), L"%s\\%s", 
			home, clippath);
    } else {
	StringCchCopy(clipdir, _countof(clipdir), clippath);
    }
    
    // Obtain the computer name.
    WCHAR name[MAX_COMPUTERNAME_LENGTH+1];
    DWORD namelen = sizeof(name);
    GetComputerName(name, &namelen);

    // Register the window class.
    ATOM atom;
    {
	WNDCLASS klass;
	ZeroMemory(&klass, sizeof(klass));
	klass.lpfnWndProc = clipWatcherWndProc;
	klass.hInstance = hInstance;
	klass.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CLIPWATCHER));
	klass.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
	klass.lpszClassName = L"ClipWatcherClass";
	atom = RegisterClass(&klass);
    }
    
    // Create a ClipWatcher object.
    ClipWatcher* watcher = CreateClipWatcher(hInstance, clipdir, clipdir, name);
    UpdateClipWatcher(watcher);
    checkFileChanges(watcher);
    
    // Create a SysTray window.
    HWND hWnd = CreateWindow(
	(LPCWSTR)atom,
	WINDOW_TITLE,
	(WS_OVERLAPPED | WS_SYSMENU),
	CW_USEDEFAULT, CW_USEDEFAULT,
	CW_USEDEFAULT, CW_USEDEFAULT,
	NULL, NULL, hInstance, watcher);
    UpdateWindow(hWnd);

    // Event loop.
    MSG msg;
    BOOL loop = TRUE;
    while (loop) {
        int n = (watcher->notifier != INVALID_HANDLE_VALUE)? 1 : 0;
	DWORD obj = MsgWaitForMultipleObjects(n, &(watcher->notifier),
                                              FALSE, INFINITE, QS_ALLINPUT);
        if (obj < WAIT_OBJECT_0) {
	    // Unexpected failure!
	    loop = FALSE;
	    break;
        }
        int i = obj - WAIT_OBJECT_0;
        if (i < n) {
            // We got a notification;
            FindNextChangeNotification(watcher->notifier);
            PostMessage(hWnd, WM_NOTIFY_FILE, 0, 0);
        } else {
            // We got a Window Message.
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    loop = FALSE;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
	}
    }

    // Clean up.
    DestroyClipWatcher(watcher);

    return (int)msg.wParam;
}


// WinMain and wmain
#ifdef WINDOWS
int WinMain(HINSTANCE hInstance, 
	    HINSTANCE hPrevInstance, 
	    LPSTR lpCmdLine,
	    int nCmdShow)
{
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    return ClipWatcherMain(hInstance, hPrevInstance, nCmdShow, argc, argv);
}
#else
int wmain(int argc, wchar_t* argv[])
{
    return ClipWatcherMain(GetModuleHandle(NULL), NULL, 0, argc, argv);
}
#endif

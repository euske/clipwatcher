//  ClipWatcher.cpp
//

#include <stdio.h>
#include <stdlib.h>
#include <Windows.h>
#include <StrSafe.h>
#include <Shlobj.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

const LPCWSTR DEFAULT_CLIPPATH = L"Dropbox\\Clipboard";
const DWORD MAX_FILE_SIZE = 32767;
const UINT WM_NOTIFY_ICON = WM_USER+1;
const UINT WM_NOTIFY_FILE = WM_NOTIFY_ICON+1;
const UINT ICON_ID = 1;
UINT CF_HOSTNAME;

typedef enum _MenuItemID {
    ITEM_NONE = 0,
    ITEM_EXIT,
    ITEM_OPEN,
};

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

static int istartswith(LPCWSTR text1, LPCWSTR text2)
{
    while (*text1 != 0 && *text2 != 0) {
	text1++;
	text2++;
    }
    return (*text2 == 0);
}

static HANDLE createGlobalText(LPCWSTR text, int nchars)
{
    HANDLE data = NULL;
    int nbytes;
    LPSTR src = getCHARfromWCHAR(text, nchars, &nbytes);
    if (src != NULL) {
	data = GlobalAlloc(GHND, nbytes+1);
	if (data != NULL) {
	    LPSTR dst = (LPSTR) GlobalLock(data);
	    if (dst != NULL) {
		CopyMemory(dst, src, nbytes+1);
		GlobalUnlock(data);
	    }
	}
	free(src);
    }
    return data;
}

static void openClipText(LPCWSTR text)
{
    if (istartswith(text, L"http://") ||
	istartswith(text, L"https://")) {
	ShellExecute(NULL, L"open", text, NULL, NULL, SW_SHOWDEFAULT);
    } else {
	// XXX
    }
}


//  FileEntry
// 
typedef struct _FileEntry {
    WCHAR name[MAX_PATH];
    FILETIME mtime;
    struct _FileEntry* next;
} FileEntry;

//  ClipWatcher
// 
typedef struct _ClipWatcher {
    HWND hwndnext;
    LPWSTR watchdir;
    LPWSTR name;
    HANDLE notifier;
    FileEntry* files;
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
    StringCbPrintf(dirpath, sizeof(dirpath), L"%s\\*.txt", 
		   watcher->watchdir);

    WIN32_FIND_DATA data;
    FileEntry* found = NULL;

    HANDLE fft = FindFirstFile(dirpath, &data);
    if (fft == NULL) goto fail;
    
    for (;;) {
	WCHAR path[MAX_PATH];
	StringCbPrintf(path, sizeof(path), L"%s\\%s", 
		       watcher->watchdir, data.cFileName);
	HANDLE fp = CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
			       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 
			       NULL);
	if (fp != INVALID_HANDLE_VALUE) {
	    FILETIME mtime;
	    if (GetFileTime(fp, NULL, NULL, &mtime)) {
		LPWSTR name = ristrip(data.cFileName, L".txt");
		if (name != NULL) {
		    FileEntry* entry = findFileEntry(watcher->files, name);
		    if (entry == NULL) {
			//fwprintf(stderr, L"added: %s\n", name);
			entry = (FileEntry*) malloc(sizeof(FileEntry));
			StringCbCopy(entry->name, sizeof(entry->name), name);
			entry->mtime = mtime;
			entry->next = watcher->files;
			watcher->files = entry;
			found = entry;
		    } else if (0 < CompareFileTime(&mtime, &entry->mtime)) {
			//fwprintf(stderr, L"updated: %s\n", name);
			entry->mtime = mtime;
			found = entry;
		    }
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

// writeClipText(watcher, text)
static void writeClipText(ClipWatcher* watcher, LPCWSTR text, int nchars)
{
    WCHAR path[MAX_PATH];
    StringCbPrintf(path, sizeof(path), L"%s\\%s.txt", 
		   watcher->watchdir, watcher->name);

    HANDLE fp = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ,
			   NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 
			   NULL);
    if (fp != INVALID_HANDLE_VALUE) {
	int nbytes;
	LPSTR bytes = getCHARfromWCHAR(text, nchars, &nbytes);
	nbytes--;
	fwprintf(stderr, L"write file: path=%s, nbytes=%d\n", path, nbytes);
	if (bytes != NULL) {
	    DWORD writtenbytes;
	    WriteFile(fp, bytes, nbytes, &writtenbytes, NULL);
	    free(bytes);
	}
	CloseHandle(fp);
    }
}

// readClipText(watcher, text)
static LPWSTR readClipText(ClipWatcher* watcher, LPCWSTR name, int* nchars)
{
    LPWSTR text = NULL;

    WCHAR path[MAX_PATH];
    StringCbPrintf(path, sizeof(path), L"%s\\%s.txt", 
		   watcher->watchdir, name);
    HANDLE fp = CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
			   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 
			   NULL);
    if (fp != INVALID_HANDLE_VALUE) {
	DWORD nbytes = GetFileSize(fp, NULL);
	if (MAX_FILE_SIZE < nbytes) {
	    nbytes = MAX_FILE_SIZE;
	}
	fwprintf(stderr, L"read file: path=%s, nbytes=%u\n", path, nbytes);
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

//  CreateClipWatcher
// 
ClipWatcher* CreateClipWatcher(LPCWSTR watchdir, LPCWSTR name)
{
    ClipWatcher* watcher = (ClipWatcher*) malloc(sizeof(ClipWatcher));
    if (watcher == NULL) return NULL;

    watcher->watchdir = wcsdup(watchdir);
    watcher->name = wcsdup(name);

    // Register a file watcher.
    watcher->notifier = FindFirstChangeNotification(
	watcher->watchdir, FALSE, 
	(FILE_NOTIFY_CHANGE_FILE_NAME |
	 FILE_NOTIFY_CHANGE_LAST_WRITE));

    watcher->files = NULL;
    
    return watcher;
}

//  DestroyClipWatcher
// 
void DestroyClipWatcher(ClipWatcher* watcher)
{
    // Unregister the file watcher;
    if (watcher->notifier != NULL) {
	FindCloseChangeNotification(watcher->notifier);
    }

    if (watcher->watchdir != NULL) {
	free(watcher->watchdir);
    }
    if (watcher->name != NULL) {
	free(watcher->name);
    }

    freeFileEntries(watcher->files);

    free(watcher);
}

// popupInfo
static void popupInfo(HWND hWnd, LPCWSTR title, LPCWSTR text)
{
    NOTIFYICONDATA nidata = {0};
    nidata.cbSize = sizeof(nidata);
    nidata.hWnd = hWnd;
    nidata.uID = ICON_ID;
    nidata.uFlags = NIF_INFO;
    nidata.dwInfoFlags = NIIF_INFO;
    nidata.uTimeout = 1;
    StringCbCopy(nidata.szInfoTitle, sizeof(nidata.szInfoTitle), title);
    StringCbCopy(nidata.szInfo, sizeof(nidata.szInfo), text);
    Shell_NotifyIcon(NIM_MODIFY, &nidata);
}

// displayContextMenu
static void displayContextMenu(HWND hWnd, POINT pt)
{
    HMENU menu = CreatePopupMenu();
    if (menu != NULL) {
	AppendMenu(menu, MF_STRING | MF_ENABLED,
		   ITEM_OPEN, L"Open");
	AppendMenu(menu, MF_STRING | MF_ENABLED,
		   ITEM_EXIT, L"Exit");
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
    switch (uMsg) {
    case WM_CREATE:
    {
	CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
	ClipWatcher* watcher = (ClipWatcher*)(cs->lpCreateParams);
	if (watcher != NULL) {
	    SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)watcher);
	    fwprintf(stderr, L"watching: %s\n", watcher->name);
	    // Insert itself into the clipboard viewer chain.
	    watcher->hwndnext = SetClipboardViewer(hWnd);
	    // Register the icon.
	    NOTIFYICONDATA nidata = {0};
	    nidata.cbSize = sizeof(nidata);
	    nidata.hWnd = hWnd;
	    nidata.uID = ICON_ID;
	    nidata.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	    nidata.uCallbackMessage = WM_NOTIFY_ICON;
	    nidata.hIcon = LoadIcon(NULL, IDI_INFORMATION);
	    StringCbPrintf(nidata.szTip, sizeof(nidata.szTip),
			   L"Watching: %s", 
			   watcher->watchdir);
	    Shell_NotifyIcon(NIM_ADD, &nidata);
	}
	return FALSE;
    }
    
    case WM_DESTROY:
    {
	LONG_PTR lp = GetWindowLongPtr(hWnd, GWLP_USERDATA);
	ClipWatcher* watcher = (ClipWatcher*)lp;
	if (watcher != NULL) {
	    // Remove itself from the clipboard viewer chain.
	    ChangeClipboardChain(hWnd, watcher->hwndnext);
	    // Unregister the icon.
	    NOTIFYICONDATA nidata = {0};
	    nidata.cbSize = sizeof(nidata);
	    nidata.hWnd = hWnd;
	    nidata.uID = ICON_ID;
	    Shell_NotifyIcon(NIM_DELETE, &nidata);
	}
	PostQuitMessage(0);
	return FALSE;
    }

    case WM_CHANGECBCHAIN:
    {
	LONG_PTR lp = GetWindowLongPtr(hWnd, GWLP_USERDATA);
	ClipWatcher* watcher = (ClipWatcher*)lp;
	if (watcher != NULL) {
	    // Update the clipboard viewer chain.
	    if ((HWND)wParam == watcher->hwndnext) {
		watcher->hwndnext = (HWND)lParam;
	    } else if (watcher->hwndnext != NULL) {
		SendMessage(watcher->hwndnext, uMsg, wParam, lParam);
	    }
	}
	return FALSE;
    }

    case WM_DRAWCLIPBOARD:
    {
	LONG_PTR lp = GetWindowLongPtr(hWnd, GWLP_USERDATA);
	ClipWatcher* watcher = (ClipWatcher*)lp;
	if (watcher != NULL) {
	    if (OpenClipboard(hWnd)) {
		fwprintf(stderr, L"clipboard changed.\n");
		HANDLE hostname = GetClipboardData(CF_HOSTNAME);
		HANDLE data = GetClipboardData(CF_TEXT);
		if (data != NULL) {
		    LPSTR bytes = (LPSTR) GlobalLock(data);
		    if (bytes != NULL) {
			int nchars;
			LPWSTR text = getWCHARfromCHAR(bytes, GlobalSize(data), &nchars);
			if (text != NULL) {
			    if (hostname == NULL) {
				writeClipText(watcher, text, nchars);
			    }
			    popupInfo(hWnd, L"Clipboard Updated", text);
			    free(text);
			}
			GlobalUnlock(data);
		    }
		}
		CloseClipboard();
	    }
	    SendMessage(watcher->hwndnext, uMsg, wParam, lParam);
	}
	return FALSE;
    }

    case WM_NOTIFY_ICON:
    {
	POINT pt;
	switch (lParam) {
	case WM_LBUTTONDBLCLK:
	    SendMessage(hWnd, WM_COMMAND, 
			MAKEWPARAM(ITEM_OPEN, 1), NULL);
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

    case WM_NOTIFY_FILE:
    {
	LONG_PTR lp = GetWindowLongPtr(hWnd, GWLP_USERDATA);
	ClipWatcher* watcher = (ClipWatcher*)lp;
	if (watcher != NULL) {
	    FileEntry* entry = checkFileChanges(watcher);
	    if (entry != NULL) {
		fwprintf(stderr, L"file changed: %s\n", entry->name);
		int nchars;
		LPWSTR text = readClipText(watcher, entry->name, &nchars);
		if (text != NULL) {
		    HANDLE data = createGlobalText(text, nchars);
		    if (data != NULL) {
			HANDLE hostname = createGlobalText(entry->name, -1);
			if (hostname != NULL) {
			    if (OpenClipboard(hWnd)) {
				SetClipboardData(CF_HOSTNAME, hostname);
				SetClipboardData(CF_TEXT, data);
				CloseClipboard();
			    }
			    GlobalFree(hostname);
			}
			GlobalFree(data);
		    }
		    free(text);
		}
	    }
	}
	return FALSE;
    }

    case WM_COMMAND:
    {
	switch (LOWORD(wParam)) {
	case ITEM_OPEN:
	    if (OpenClipboard(hWnd)) {
		HANDLE data = GetClipboardData(CF_TEXT);
		if (data != NULL) {
		    LPSTR bytes = (LPSTR) GlobalLock(data);
		    if (bytes != NULL) {
			int nchars;
			LPWSTR text = getWCHARfromCHAR(bytes, GlobalSize(data), &nchars);
			if (text != NULL) {
			    openClipText(text);
			    free(text);
			}
			GlobalUnlock(data);
		    }
		}
		CloseClipboard();
	    }
	    break;
	case ITEM_EXIT:
	    SendMessage(hWnd, WM_CLOSE, 0, 0);
	    break;
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

    // Obtain the home path.
    WCHAR home[MAX_PATH];
    SHGetFolderPath(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, home);

    // Obtain the clipboard directory path.
    WCHAR clipdir[MAX_PATH];
    StringCbPrintf(clipdir, sizeof(clipdir), L"%s\\%s", 
		   home, clippath);

    // Obtain the computer name.
    WCHAR name[MAX_COMPUTERNAME_LENGTH+1];
    DWORD namelen = sizeof(name);
    GetComputerName(name, &namelen);

    // Register a clipboard format.
    CF_HOSTNAME = RegisterClipboardFormat(L"ClipWatcherHostname");

    // Register the window class.
    ATOM atom;
    {
	WNDCLASS klass;
	ZeroMemory(&klass, sizeof(klass));
	klass.lpfnWndProc = clipWatcherWndProc;
	klass.hInstance = hInstance;
	klass.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
	klass.lpszClassName = L"ClipWatcherClass";
	atom = RegisterClass(&klass);
    }

    // Create a ClipWatcher object.
    ClipWatcher* watcher = CreateClipWatcher(clipdir, name);
    checkFileChanges(watcher);

    // Create a SysTray window.
    HWND hWnd = CreateWindow(
	(LPCWSTR)atom,
	L"ClipWatcher", 
	(WS_OVERLAPPED | WS_SYSMENU),
	CW_USEDEFAULT, CW_USEDEFAULT,
	CW_USEDEFAULT, CW_USEDEFAULT,
	NULL, NULL, hInstance, watcher);
    UpdateWindow(hWnd);

    // Event loop.
    MSG msg;
    BOOL loop = TRUE;
    while (loop) {
	DWORD obj = MsgWaitForMultipleObjects(1, &watcher->notifier,
					      FALSE, INFINITE, QS_ALLINPUT);
	switch (obj) {
	case WAIT_OBJECT_0:
	    // We got a notification;
	    FindNextChangeNotification(watcher->notifier);
	    PostMessage(hWnd, WM_NOTIFY_FILE, 0, 0);
	    break;
	case WAIT_OBJECT_0+1:
	    // We got a Window Message.
	    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
		if (msg.message == WM_QUIT) {
		    loop = FALSE;
		    break;
		}
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	    }
	    break;
	default:
	    // Unexpected failure!
	    loop = FALSE;
	    break;
	}
    }

    // Clean up.
    DestroyClipWatcher(watcher);

    return msg.wParam;
}


// WinMain and wmain

int WinMain(HINSTANCE hInstance, 
	    HINSTANCE hPrevInstance, 
	    LPSTR lpCmdLine,
	    int nCmdShow)
{
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    return ClipWatcherMain(hInstance, hPrevInstance, nCmdShow, argc, argv);
}

int wmain(int argc, wchar_t* argv[])
{
    return ClipWatcherMain(GetModuleHandle(NULL), NULL, 0, argc, argv);
}

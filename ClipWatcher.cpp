//  ClipWatcher.cpp
//

#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <Windows.h>
#include <StrSafe.h>
#include <Shlobj.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

const UINT WM_NOTIFY_ICON = WM_USER+1;
const UINT WM_NOTIFY_FILE = WM_NOTIFY_ICON+1;

typedef enum _MenuItemID {
    ITEM_NONE = 0,
    ITEM_EXIT,
    ITEM_OPEN,
};

const UINT ICON_ID = 1;

// 
typedef struct _FileEntry {
    TCHAR name[MAX_PATH];
    FILETIME mtime;
    struct _FileEntry* next;
} FileEntry;

//  ClipWatcher
// 
typedef struct _ClipWatcher {
    HWND hwndnext;
    LPTSTR watchdir;
    LPTSTR name;
    HANDLE notifier;
    BOOL writefile;
    FileEntry* files;
} ClipWatcher;

// findFileEntry(files, name)
static FileEntry* findFileEntry(FileEntry* entry, LPCTSTR name)
{
    while (entry != NULL) {
	if (_tcsicmp(entry->name, name) == 0) return entry;
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
    TCHAR dirpath[MAX_PATH];
    StringCbPrintf(dirpath, sizeof(dirpath), _T("%s\\*.txt"), 
		   watcher->watchdir);
    WIN32_FIND_DATA data;
    FileEntry* found = NULL;

    HANDLE fft = FindFirstFile(dirpath, &data);
    if (fft == NULL) goto fail;
    
    for (;;) {
	_ftprintf(stderr, _T("name=%s\n"), data.cFileName);
	TCHAR path[MAX_PATH];
	StringCbPrintf(path, sizeof(path), _T("%s\\%s"), 
		       watcher->watchdir, data.cFileName);
	HANDLE fp = CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
			       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 
			       NULL);
	if (fp != NULL) {
	    FILETIME mtime;
	    GetFileTime(fp, NULL, NULL, &mtime);
	    CloseHandle(fp);
	    FileEntry* entry = findFileEntry(watcher->files, data.cFileName);
	    if (entry == NULL) {
		entry = (FileEntry*) malloc(sizeof(FileEntry));
		StringCbCopy(entry->name, sizeof(entry->name), data.cFileName);
		entry->mtime = mtime;
		entry->next = watcher->files;
		watcher->files = entry;
		found = entry;
	    } else if (0 < CompareFileTime(&mtime, &entry->mtime)) {
		entry->mtime = mtime;
		found = entry;
	    }
	}
	if (!FindNextFile(fft, &data)) break;
    }
    FindClose(fft);

fail:
    return found;
}

// writeClipText(watcher, text)
static void writeClipText(ClipWatcher* watcher, LPCTSTR text)
{
    TCHAR path[MAX_PATH];
    StringCbPrintf(path, sizeof(path), _T("%s\\%s.txt"), 
		   watcher->watchdir, watcher->name);

    HANDLE fp = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ,
			   NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 
			   NULL);
    if (fp != NULL) {
	_ftprintf(stderr, _T("write file: path=%s\n"), path);
	BYTE* rawbytes;
#ifdef UNICODE
	int nbytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
	bytes = (BYTE)* malloc(sizeof(BYTE)*(nbytes+1));
	WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes, nbytes, NULL, NULL);
#else
	int nbytes = _tcslen(text);
	rawbytes = (BYTE*)text;
#endif
	DWORD written;
	WriteFile(fp, rawbytes, nbytes, &written, NULL);
	CloseHandle(fp);
    }
}

// readClipText(watcher, text)
static LPTSTR readClipText(ClipWatcher* watcher, LPCTSTR name)
{
    TCHAR path[MAX_PATH];
    StringCbPrintf(path, sizeof(path), _T("%s\\%s"), 
		   watcher->watchdir, name);
    
    HANDLE fp = CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
			   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 
			   NULL);
    LPTSTR text = NULL;
    WCHAR* chars = NULL;
    BYTE* bytes = NULL;
    if (fp != NULL) {
	DWORD nbytes = GetFileSize(fp, NULL);
	bytes = (BYTE*) malloc(nbytes+1);
	if (bytes != NULL) {
	    DWORD read;
	    ReadFile(fp, bytes, nbytes, &read, NULL);
	    int nchars = MultiByteToWideChar(CP_UTF8, 0, (LPSTR)bytes, nbytes, NULL, 0);
	    chars = (WCHAR*) malloc(sizeof(WCHAR)*(nchars+1));
	    if (chars != NULL) {
		MultiByteToWideChar(CP_UTF8, 0, (LPSTR)bytes, nbytes, chars, nchars);
	    }
	}
	CloseHandle(fp);
    }

    if (chars != NULL) {
#ifdef UNICODE
	text = chars;
#else
	int textlen = WideCharToMultiByte(CP_ACP, 0, chars, -1, NULL, 0, NULL, NULL);
	text = (LPTSTR) malloc(sizeof(TCHAR)*(textlen+1));
	WideCharToMultiByte(CP_ACP, 0, chars, -1, text, textlen, NULL, NULL);
#endif
    }
    return text;
}

//  CreateClipWatcher
// 
ClipWatcher* CreateClipWatcher(LPCTSTR watchdir, LPCTSTR name)
{
    ClipWatcher* watcher = (ClipWatcher*) malloc(sizeof(ClipWatcher));
    if (watcher == NULL) return NULL;

    watcher->watchdir = _tcsdup(watchdir);
    watcher->name = _tcsdup(name);

    // Register a file watcher.
    watcher->notifier = FindFirstChangeNotification(
	watcher->watchdir, FALSE, 
	(FILE_NOTIFY_CHANGE_FILE_NAME |
	 FILE_NOTIFY_CHANGE_LAST_WRITE));

    watcher->writefile = TRUE;
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
static void popupInfo(HWND hWnd, LPCTSTR title, LPCTSTR text)
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
		   ITEM_OPEN, _T("Open"));
	AppendMenu(menu, MF_STRING | MF_ENABLED,
		   ITEM_EXIT, _T("Exit"));
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
			   _T("Watching: %s"), 
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
		HANDLE data = GetClipboardData(CF_TEXT);
		if (data != NULL) {
		    _ftprintf(stderr, _T("open clip\n"));
		    LPCSTR rawtext = (LPCSTR) GlobalLock(data);
		    if (rawtext != NULL) {
			LPTSTR text;
#ifdef UNICODE
			int nchars = MultiByteToWideChar(CP_ACP, 0, rawtext, -1, NULL, 0);
			text = (LPTSTR)malloc(sizeof(TCHAR)*(nchars+1));
			MultiByteToWideChar(CP_ACP, 0, rawtext, -1, text, nchars);
#else
			text = (LPTSTR)rawtext;
#endif
			if (watcher->writefile) {
			    writeClipText(watcher, text);
			}
			watcher->writefile = TRUE;
			popupInfo(hWnd, _T("Clipboard Updated"), text);
#ifdef UNICODE
			free(text);
#endif
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
		LPTSTR text = readClipText(watcher, entry->name);
		if (text != NULL) {
		    int nbytes;
		    HANDLE data;
#ifdef UNICODE
		    nbytes = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL);
#else
		    nbytes = _tcslen(text)+1;
#endif
		    data = GlobalAlloc(GHND, nbytes);
		    if (data != NULL) {
			BYTE* bytes = (BYTE*) GlobalLock(data);
#ifdef UNICODE
			MultiByteToWideChar(CP_UTF8, 0, text, -1, bytes, nbytes, NULL, NULL);
#else
			StringCbCopy((LPSTR)bytes, nbytes, text);
#endif
			if (OpenClipboard(hWnd)) {
			    watcher->writefile = FALSE;
			    SetClipboardData(CF_TEXT, data);
			    CloseClipboard();
			}
			GlobalUnlock(data);
			GlobalFree(data);
		    }
		    free(text);
		}
		
		TCHAR path[MAX_PATH];
		StringCbPrintf(path, sizeof(path), _T("%s\\%s"), 
			       watcher->watchdir, entry->name);
		_ftprintf(stderr, _T("open file: path=%s\n"), path);
		HANDLE fp = CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
				       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 
				       NULL);
		if (fp != NULL) {
		    CloseHandle(fp);
		}
	    }
	}
	return FALSE;
    }

    case WM_COMMAND:
    {
	switch (LOWORD(wParam)) {
	case ITEM_OPEN:
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


//  WinMain
// 
int WinMain(HINSTANCE hInstance, 
	    HINSTANCE hPrevInstance, 
	    LPSTR lpCmdLine,
	    int nCmdShow)
{
    LPCTSTR WATCHPATH = _T("Dropbox\\Clipboard");

    TCHAR home[MAX_PATH];
    SHGetFolderPath(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, home);

    TCHAR watchdir[MAX_PATH];
    _stprintf_s(watchdir, sizeof(watchdir)/sizeof(watchdir[0]), 
		_T("%s\\%s"), home, WATCHPATH);

    TCHAR name[MAX_COMPUTERNAME_LENGTH+1];
    DWORD namelen = sizeof(name);
    GetComputerName(name, &namelen);
    ClipWatcher* watcher = CreateClipWatcher(watchdir, name);

    ATOM atom;
    {
	WNDCLASS klass;
	ZeroMemory(&klass, sizeof(klass));
	klass.lpfnWndProc = clipWatcherWndProc;
	klass.hInstance = hInstance;
	klass.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
	klass.lpszClassName = _T("ClipWatcherClass");
	atom = RegisterClass(&klass);
    }

    HWND hWnd = CreateWindow(
	(LPCTSTR)atom,
	_T("ClipWatcher"), 
	(WS_OVERLAPPED | WS_SYSMENU),
	CW_USEDEFAULT, CW_USEDEFAULT,
	CW_USEDEFAULT, CW_USEDEFAULT,
	NULL, NULL, hInstance, watcher);
    UpdateWindow(hWnd);

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

    DestroyClipWatcher(watcher);

    return msg.wParam;
}



int main(int argc, char* argv[])
{
    return WinMain(GetModuleHandle(NULL), NULL, NULL, 0);
}

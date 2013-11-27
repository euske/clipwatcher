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

// Constants (you shouldn't change)
const LPCWSTR CLIPWATCHER_NAME = L"ClipWatcher";
const LPCWSTR CLIPWATCHER_WNDCLASS = L"ClipWatcherClass";
const LPCWSTR CLIPWATCHER_ORIGIN = L"ClipWatcherOrigin";
const WORD BMP_SIGNATURE = 0x4d42; // 'BM' in little endian.
static UINT CF_ORIGIN;
enum {
    WM_NOTIFY_ICON = WM_USER+1,
    WM_NOTIFY_FILE,
};
const LPCWSTR FILE_EXT_TEXT = L".txt";
const LPCWSTR FILE_EXT_BITMAP = L".bmp";
enum {
    FILETYPE_TEXT = 0,
    FILETYPE_BITMAP = 1,
};

// Constants (you may change)
const int CLIPBOARD_RETRY = 3;
const UINT CLIPBOARD_DELAY = 100;
const UINT ICON_BLINK_INTERVAL = 400;
const UINT ICON_BLINK_COUNT = 10;
const UINT FILESYSTEM_INTERVAL = 1000;

// Resources (loaded at runtime)
static HICON HICON_EMPTY;
static HICON HICON_FILETYPE[2];
static WCHAR DEFAULT_CLIPPATH[100] = L"Clipboard";
static WCHAR MESSAGE_WATCHING[100] = L"Watching: %s";
static WCHAR MESSAGE_UPDATED[100] = L"Clipboard Updated";
static WCHAR MESSAGE_BITMAP[100] = L"Bitmap";

static FILE* logfp = stderr;

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

// setClipboardOrigin(path)
static void setClipboardOrigin(LPCWSTR path)
{
    int nbytes;
    LPSTR src = getCHARfromWCHAR(path, wcslen(path), &nbytes);
    if (src != NULL) {
	HANDLE data = GlobalAlloc(GHND, nbytes+1);
	if (data != NULL) {
	    LPSTR dst = (LPSTR) GlobalLock(data);
	    if (dst != NULL) {
		CopyMemory(dst, src, nbytes+1);
		GlobalUnlock(data);
                SetClipboardData(CF_ORIGIN, data);
                data = NULL;
	    }
            if (data != NULL) {
                GlobalFree(data);
            }
	}
	free(src);
    }
}

// setClipboardText(text, nchars)
static void setClipboardText(LPCWSTR text, int nchars)
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
                SetClipboardData(CF_TEXT, data);
                data = NULL;
	    }
            if (data != NULL) {
                GlobalFree(data);
            }
	}
	free(src);
    }
}

// setClipboardDIB(bmp)
static void setClipboardDIB(BITMAPINFO* src)
{
    size_t nbytes = getBMPSize(src);
    HANDLE data = GlobalAlloc(GHND, nbytes);
    if (data != NULL) {
        LPVOID dst = GlobalLock(data);
        if (dst != NULL) {
            CopyMemory(dst, src, nbytes);
            GlobalUnlock(data);
            SetClipboardData(CF_DIB, data);
            data = NULL;
        }
        if (data != NULL) {
            GlobalFree(data);
        }
    }
}

// getClipboardText(buf, buflen)
static int getClipboardText(LPWSTR buf, int buflen)
{
    int filetype = -1;

    // CF_TEXT
    HANDLE data = GetClipboardData(CF_TEXT);
    if (data != NULL) {
        LPSTR bytes = (LPSTR) GlobalLock(data);
        if (bytes != NULL) {
            int nchars;
            LPWSTR text = getWCHARfromCHAR(bytes, GlobalSize(data), &nchars);
            if (text != NULL) {
                filetype = FILETYPE_TEXT;
                StringCchCopy(buf, buflen, text);
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
            filetype = FILETYPE_BITMAP;
            StringCchCopy(buf, buflen, MESSAGE_BITMAP);
            GlobalUnlock(bytes);
        }
    }
    
    return filetype;
}

// writeBytes(path, bytes, nbytes)
static void writeBytes(LPCWSTR path, LPVOID bytes, int nbytes)
{
    HANDLE fp = CreateFile(path, GENERIC_WRITE, 0,
			   NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 
			   NULL);
    if (fp != INVALID_HANDLE_VALUE) {
	fwprintf(logfp, L"write: path=%s, nbytes=%u\n", path, nbytes);
        DWORD writtenbytes;
        WriteFile(fp, bytes, nbytes, &writtenbytes, NULL);
	CloseHandle(fp);
    }
}

// writeTextFile(path, text, nchars)
static void writeTextFile(LPCWSTR path, LPCWSTR text, int nchars)
{
    int nbytes;
    LPSTR bytes = getCHARfromWCHAR(text, nchars, &nbytes);
    nbytes--;
    if (bytes != NULL) {
        writeBytes(path, bytes, nbytes);
        free(bytes);
    }
}

// readTextFile(path, &nchars)
static LPWSTR readTextFile(LPCWSTR path, int* nchars)
{
    const DWORD MAX_FILE_SIZE = 32767;

    LPWSTR text = NULL;
    HANDLE fp = CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
			   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 
			   NULL);
    if (fp != INVALID_HANDLE_VALUE) {
	DWORD nbytes = GetFileSize(fp, NULL);
	if (MAX_FILE_SIZE < nbytes) {
	    nbytes = MAX_FILE_SIZE;
	}
	fwprintf(logfp, L"read: path=%s, nbytes=%u\n", path, nbytes);
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

// writeBMPFile(path, bytes, nbytes)
static void writeBMPFile(LPCWSTR path, LPVOID bytes, SIZE_T nbytes)
{
    BITMAPFILEHEADER filehdr = {0};
    filehdr.bfType = BMP_SIGNATURE;
    filehdr.bfSize = sizeof(filehdr)+nbytes;
    filehdr.bfOffBits = sizeof(filehdr)+getBMPHeaderSize((BITMAPINFO*)bytes);
    HANDLE fp = CreateFile(path, GENERIC_WRITE, 0,
			   NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 
			   NULL);
    if (fp != INVALID_HANDLE_VALUE) {
	fwprintf(logfp, L"write: path=%s, nbytes=%u\n", path, filehdr.bfSize);
        DWORD writtenbytes;
        WriteFile(fp, &filehdr, sizeof(filehdr), &writtenbytes, NULL);
        WriteFile(fp, bytes, nbytes, &writtenbytes, NULL);
	CloseHandle(fp);
    }
}

// readBMPFile(path, &nchars)
static BITMAPINFO* readBMPFile(LPCWSTR path)
{
    BITMAPINFO* bmp = NULL;
    HANDLE fp = CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
			   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 
			   NULL);
    if (fp != INVALID_HANDLE_VALUE) {
        DWORD readbytes;
        BITMAPFILEHEADER filehdr;
        ReadFile(fp, &filehdr, sizeof(filehdr), &readbytes, NULL);
        if (filehdr.bfType == BMP_SIGNATURE) {
            DWORD bmpsize = filehdr.bfSize - sizeof(filehdr);
            bmp = (BITMAPINFO*) malloc(bmpsize);
            if (bmp != NULL) {
                ReadFile(fp, bmp, bmpsize, &readbytes, NULL);
            }
	}
	CloseHandle(fp);
    }

    return bmp;
}

// openClipFile()
static BOOL openClipFile()
{
    const LPCWSTR OP_OPEN = L"open";

    BOOL success = FALSE;

    HANDLE data = GetClipboardData(CF_TEXT);
    if (data != NULL) {
        LPSTR bytes = (LPSTR) GlobalLock(data);
        if (bytes != NULL) {
            int nchars;
            LPWSTR text = getWCHARfromCHAR(bytes, GlobalSize(data), &nchars);
            if (text != NULL) {
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
                        fwprintf(logfp, L"open: url=%s\n", url);
                        ShellExecute(NULL, OP_OPEN, url, NULL, NULL, SW_SHOWDEFAULT);
                        success = TRUE;
                        free(url);
                    }
                }
                free(text);
            }
            GlobalUnlock(data);
        }
    }
    
    if (success) return success;

    data = GetClipboardData(CF_ORIGIN);
    if (data != NULL) {
        LPSTR bytes = (LPSTR) GlobalLock(data);
        if (bytes != NULL) {
            int nchars;
            LPWSTR path = getWCHARfromCHAR(bytes, GlobalSize(data), &nchars);
            if (path != NULL) {
                fwprintf(logfp, L"open: path=%s\n", path);
                ShellExecute(NULL, OP_OPEN, path, NULL, NULL, SW_SHOWDEFAULT);
                success = TRUE;
                free(path);
            }
            GlobalUnlock(data);
        }
    }

    return success;
}

// exportClipFile(basepath)
static void exportClipFile(LPCWSTR basepath)
{
    // CF_TEXT
    HANDLE data = GetClipboardData(CF_TEXT);
    if (data != NULL) {
        LPSTR bytes = (LPSTR) GlobalLock(data);
        if (bytes != NULL) {
            int nchars;
            LPWSTR text = getWCHARfromCHAR(bytes, GlobalSize(data), &nchars);
            if (text != NULL) {
                WCHAR path[MAX_PATH];
                StringCchPrintf(path, _countof(path), L"%s.txt", basepath);
                setClipboardOrigin(path);
                writeTextFile(path, text, nchars);
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
            StringCchPrintf(path, _countof(path), L"%s.bmp", basepath);
            setClipboardOrigin(path);
            writeBMPFile(path, bytes, nbytes);
            GlobalUnlock(bytes);
        }
    }
}

// getFileHash(fp)
static DWORD getFileHash(HANDLE fp, DWORD n)
{
    DWORD hash = 0;
    DWORD bufsize = sizeof(DWORD)*n;
    DWORD* buf = (DWORD*) malloc(bufsize);
    if (buf != NULL) {
	DWORD readbytes;
	ZeroMemory(buf, bufsize);
	ReadFile(fp, buf, bufsize, &readbytes, NULL);
	for (int i = 0; i < n; i++) {
	    hash ^= buf[i];
	}
	free(buf);
    }
    return hash;
}


//  FileEntry
// 
typedef struct _FileEntry {
    WCHAR path[MAX_PATH];
    DWORD hash;
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

    UINT icon_id;
    UINT_PTR blink_timer_id;
    UINT_PTR check_timer_id;
    HICON icon_blinking;
    int icon_blink_count;
} ClipWatcher;

// findFileEntry(files, path)
static FileEntry* findFileEntry(FileEntry* entry, LPCWSTR path)
{
    while (entry != NULL) {
	if (wcsicmp(entry->path, path) == 0) return entry;
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
        if (0 <= index && wcsnicmp(name, watcher->name, index) != 0) {
            WCHAR path[MAX_PATH];
            StringCchPrintf(path, _countof(path), L"%s\\%s", 
                            watcher->srcdir, name);
            HANDLE fp = CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, 
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING,
                                   NULL);
            if (fp != INVALID_HANDLE_VALUE) {
                DWORD hash = getFileHash(fp, 256);
                fwprintf(logfp, L"check: name=%s (%08x)\n", name, hash);
                FileEntry* entry = findFileEntry(watcher->files, path);
                if (entry == NULL) {
                    fwprintf(logfp, L"added: name=%s\n", name);
                    entry = (FileEntry*) malloc(sizeof(FileEntry));
                    StringCchCopy(entry->path, _countof(entry->path), path);
                    entry->hash = hash;
                    entry->next = watcher->files;
                    watcher->files = entry;
                    found = entry;
                } else if (hash != entry->hash) {
                    fwprintf(logfp, L"updated: name=%s\n", name);
                    entry->hash = hash;
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

    watcher->icon_id = 1;
    watcher->blink_timer_id = 1;
    watcher->check_timer_id = 2;
    watcher->icon_blinking = NULL;
    watcher->icon_blink_count = 0;
    return watcher;
}

//  StartClipWatcher
// 
void StartClipWatcher(ClipWatcher* watcher)
{
    if (watcher->notifier == INVALID_HANDLE_VALUE) {
        watcher->notifier = FindFirstChangeNotification(
            watcher->srcdir, FALSE, 
            (FILE_NOTIFY_CHANGE_FILE_NAME |
             FILE_NOTIFY_CHANGE_SIZE |
             FILE_NOTIFY_CHANGE_ATTRIBUTES |
             FILE_NOTIFY_CHANGE_LAST_WRITE));
        fwprintf(logfp, L"register: srcdir=%s, notifier=%p\n", 
                 watcher->srcdir, watcher->notifier);
    }
}

//  StopClipWatcher
// 
void StopClipWatcher(ClipWatcher* watcher)
{
    if (watcher->notifier != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(watcher->notifier);
        watcher->notifier = INVALID_HANDLE_VALUE;
    }
}

//  DestroyClipWatcher
// 
void DestroyClipWatcher(ClipWatcher* watcher)
{
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
	    nidata.hIcon = HICON_EMPTY;
	    StringCchPrintf(nidata.szTip, _countof(nidata.szTip),
			    MESSAGE_WATCHING, watcher->srcdir);
	    Shell_NotifyIcon(NIM_ADD, &nidata);
            SetTimer(hWnd, watcher->blink_timer_id, ICON_BLINK_INTERVAL, NULL);
            SetTimer(hWnd, watcher->check_timer_id, FILESYSTEM_INTERVAL, NULL);
	}
	return FALSE;
    }
    
    case WM_DESTROY:
    {
        // Clean up.
	LONG_PTR lp = GetWindowLongPtr(hWnd, GWLP_USERDATA);
	ClipWatcher* watcher = (ClipWatcher*)lp;
	if (watcher != NULL) {
            KillTimer(hWnd, watcher->blink_timer_id);
            KillTimer(hWnd, watcher->check_timer_id);
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
                fwprintf(logfp, L"updated clipboard: seqno=%d\n", seqno);
                for (int i = 0; i < CLIPBOARD_RETRY; i++) {
                    Sleep(CLIPBOARD_DELAY);
                    if (OpenClipboard(hWnd)) {
                        if (GetClipboardData(CF_ORIGIN) == NULL) {
                            WCHAR path[MAX_PATH];
                            StringCchPrintf(path, _countof(path), L"%s\\%s", 
                                            watcher->dstdir, watcher->name);
                            exportClipFile(path);
                        }
                        WCHAR text[256];
                        int filetype = getClipboardText(text, _countof(text));
                        if (0 <= filetype) {
                            NOTIFYICONDATA nidata = {0};
                            nidata.cbSize = sizeof(nidata);
                            nidata.hWnd = hWnd;
                            nidata.uID = watcher->icon_id;
                            nidata.uFlags = NIF_INFO;
                            nidata.dwInfoFlags = NIIF_INFO;
                            nidata.uTimeout = 1;
                            StringCchCopy(nidata.szInfoTitle, 
                                          _countof(nidata.szInfoTitle), 
                                          MESSAGE_UPDATED);
                            StringCchCopy(nidata.szInfo, 
                                          _countof(nidata.szInfo),
                                          text);
                            Shell_NotifyIcon(NIM_MODIFY, &nidata);
                            watcher->icon_blinking = HICON_FILETYPE[filetype];
                            watcher->icon_blink_count = ICON_BLINK_COUNT;
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
		fwprintf(logfp, L"updated file: path=%s\n", entry->path);
                int index = rindex(entry->path, L'.');
                if (0 <= index) {
                    WCHAR* ext = &(entry->path[index]);
                    if (_wcsicmp(ext, FILE_EXT_TEXT) == 0) {
                        // CF_TEXT
                        int nchars;
                        LPWSTR text = readTextFile(entry->path, &nchars);
                        if (text != NULL) {
                            if (OpenClipboard(hWnd)) {
                                EmptyClipboard();
                                setClipboardOrigin(entry->path);
                                setClipboardText(text, nchars);
                                CloseClipboard();
                            }
                            free(text);
                        }
                    } else if (_wcsicmp(ext, FILE_EXT_BITMAP) == 0) {
                        // CF_DIB
                        BITMAPINFO* bmp = readBMPFile(entry->path);
                        if (bmp != NULL) {
                            if (OpenClipboard(hWnd)) {
                                EmptyClipboard();
                                setClipboardOrigin(entry->path);
                                setClipboardDIB(bmp);
                                CloseClipboard();
                            }
                            free(bmp);
                        }
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
                    openClipFile();
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
            StopClipWatcher(watcher);
        }
        return TRUE;
    }

    case WM_NOTIFY_ICON:
    {
        // UI event handling.
	POINT pt;
        HMENU menu = GetMenu(hWnd);
        if (menu != NULL) {
            menu = GetSubMenu(menu, 0);
        }
	switch (lParam) {
	case WM_LBUTTONDBLCLK:
            if (menu != NULL) {
                UINT item = GetMenuDefaultItem(menu, FALSE, 0);
                SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(item, 1), NULL);
            }
	    break;
	case WM_LBUTTONUP:
	    break;
	case WM_RBUTTONUP:
	    if (GetCursorPos(&pt)) {
                SetForegroundWindow(hWnd);
                if (menu != NULL) {
                    TrackPopupMenu(menu, TPM_LEFTALIGN, 
                                   pt.x, pt.y, 0, hWnd, NULL);
                }
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
            UINT_PTR timer_id = wParam;
            if (timer_id == watcher->blink_timer_id) {
                // Blink the icon.
                if (watcher->icon_blink_count) {
                    watcher->icon_blink_count--;
                    BOOL on = (watcher->icon_blink_count % 2);
                    NOTIFYICONDATA nidata = {0};
                    nidata.cbSize = sizeof(nidata);
                    nidata.hWnd = hWnd;
                    nidata.uID = watcher->icon_id;
                    nidata.uFlags = NIF_ICON;
                    nidata.hIcon = (on? watcher->icon_blinking : HICON_EMPTY);
                    Shell_NotifyIcon(NIM_MODIFY, &nidata);
                }
            } else if (timer_id == watcher->check_timer_id) {
                // Check the filesystem.
                StartClipWatcher(watcher);
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
    HANDLE mutex = CreateMutex(NULL, TRUE, CLIPWATCHER_NAME);
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
        klass.lpszMenuName = MAKEINTRESOURCE(IDM_POPUPMENU);
	klass.lpszClassName = CLIPWATCHER_WNDCLASS;
	atom = RegisterClass(&klass);
    }
    
    // Register the clipboard format.
    CF_ORIGIN = RegisterClipboardFormat(CLIPWATCHER_ORIGIN);

    // Load the resources.
    HICON_EMPTY = \
        LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CLIPEMPTY));
    HICON_FILETYPE[FILETYPE_TEXT] = \
        LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CLIPTEXT));
    HICON_FILETYPE[FILETYPE_BITMAP] = \
        LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CLIPBITMAP));
    LoadString(hInstance, IDS_DEFAULT_CLIPPATH, 
               DEFAULT_CLIPPATH, _countof(DEFAULT_CLIPPATH));
    LoadString(hInstance, IDS_MESSAGE_WATCHING, 
               MESSAGE_WATCHING, _countof(MESSAGE_WATCHING));
    LoadString(hInstance, IDS_MESSAGE_UPDATED, 
               MESSAGE_UPDATED, _countof(MESSAGE_UPDATED));
    LoadString(hInstance, IDS_MESSAGE_BITMAP, 
               MESSAGE_BITMAP, _countof(MESSAGE_BITMAP));
    
    // Create a ClipWatcher object.
    ClipWatcher* watcher = CreateClipWatcher(clipdir, clipdir, name);
    StartClipWatcher(watcher);
    checkFileChanges(watcher);
    
    // Create a SysTray window.
    HWND hWnd = CreateWindow(
	(LPCWSTR)atom,
	CLIPWATCHER_NAME,
	(WS_OVERLAPPED | WS_SYSMENU),
	CW_USEDEFAULT, CW_USEDEFAULT,
	CW_USEDEFAULT, CW_USEDEFAULT,
	NULL, NULL, hInstance, watcher);
    UpdateWindow(hWnd);
    {
        // Set the default item.
        HMENU menu = GetMenu(hWnd);
        if (menu != NULL) {
            menu = GetSubMenu(menu, 0);
            if (menu != NULL) {
                SetMenuDefaultItem(menu, IDM_OPEN, FALSE);
            }
        }
    }

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
    StopClipWatcher(watcher);
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

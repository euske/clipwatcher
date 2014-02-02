# Makefile

DEL=del /f
COPY=copy /y
MT=mt -nologo
CL=cl /nologo
RC=rc
LINK=link /nologo

CFLAGS=/MD /O2 /GA /Zi
LDFLAGS=/DEBUG /OPT:REF /OPT:ICF
RCFLAGS=
DEFS_COMMON=/D WIN32 /D UNICODE /D _UNICODE
DEFS_CONSOLE=$(DEFS_COMMON) /D CONSOLE /D _CONSOLE
DEFS_WINDOWS=$(DEFS_COMMON) /D WINDOWS /D _WINDOWS
DEFS=$(DEFS_CONSOLE)
LIBS=
INCLUDES=
TARGET=ClipWatcher.exe

CLIPDIR=Z:\tmp\Clipboard
DESTDIR=%UserProfile%\bin

all: $(TARGET)

install: clean
	$(MAKE) $(TARGET) DEFS="$(DEFS_WINDOWS)"
	$(COPY) $(TARGET) $(DESTDIR)

test: $(TARGET)
	.\ClipWatcher.exe $(CLIPDIR)

clean:
	-$(DEL) $(TARGET)
	-$(DEL) *.lib *.exp *.obj *.res *.ilk *.pdb *.manifest

ClipWatcher.exe: ClipWatcher.obj ClipWatcher.res
	$(LINK) $(LDFLAGS) /manifest /out:$@ $** $(LIBS)
	$(MT) -manifest $@.manifest -outputresource:$@;1

ClipWatcher.cpp: Resource.h
ClipWatcher.rc: Resource.h ClipWatcher.ico ClipEmpty.ico ClipText.ico ClipBitmap.ico

.cpp.obj:
	$(CL) $(CFLAGS) /Fo$@ /c $< $(DEFS) $(INCLUDES)
.rc.res:
	$(RC) $(RCFLAGS) $<

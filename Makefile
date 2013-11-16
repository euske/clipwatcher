# Makefile

DEL=del /f
COPY=copy /y
MT=mt -nologo
CL=cl /nologo
LINK=link /nologo

CFLAGS=/MD /O2 /GA /Zi
LDFLAGS=/DEBUG /OPT:REF /OPT:ICF
RCFLAGS=
DEFS_COMMON=/D WIN32 /D UNICODE /D _UNICODE
DEFS_CONSOLE=$(DEFS_COMMON) /D CONSOLE /D _CONSOLE
DEFS_WINDOWS=$(DEFS_COMMON) /D WINDOWS /D _WINDOWS
DEFS=$(DEFS_WINDOWS)
LIBS=
INCLUDES=
TARGET=ClipWatcher.exe
OBJS=ClipWatcher.obj ClipWatcher.res

CLIPDIR=Z:\tmp\Clipboard
DESTDIR=%UserProfile%\bin

all: $(TARGET)

install: $(TARGET)
	$(COPY) $(TARGET) $(DESTDIR)

test:
	$(MAKE) $(TARGET) DEFS="$(DEFS_CONSOLE)"
	.\$(TARGET) $(CLIPDIR)

clean:
	-$(DEL) $(TARGET)
	-$(DEL) *.obj *.res *.ilk *.pdb *.manifest

$(TARGET): $(OBJS)
	$(LINK) $(LDFLAGS) /manifest /out:$@ $** $(LIBS)
	$(MT) -manifest $@.manifest -outputresource:$@;1

ClipWatcher.cpp: Resource.h
ClipWatcher.rc: Resource.h ClipWatcher.ico

.cpp.obj:
	$(CL) $(CFLAGS) /Fo$@ /c $< $(DEFS) $(INCLUDES)
.rc.res:
	$(RC) $(RCFLAGS) $<

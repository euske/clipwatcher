# Makefile

DEL=del /f
MT=mt -nologo
CL=cl /nologo
LINK=link /nologo

CFLAGS=/MD /O2 /GA /Zi
LDFLAGS=/DEBUG /OPT:REF /OPT:ICF
RCFLAGS=
DEFS=/D WIN32 /D UNICODE /D _UNICODE /D WINDOWS /D _WINDOWS
LIBS=
INCLUDES=
TARGET=ClipWatcher.exe
OBJS=ClipWatcher.obj ClipWatcher.res

all: $(TARGET)

test: $(TARGET)
	.\$(TARGET)

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

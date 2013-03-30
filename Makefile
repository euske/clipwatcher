# Makefile

DEL=del /f
MT=mt -nologo
CL=cl /nologo
LINK=link /nologo

CFLAGS=
DEFS=/D WIN32 /D WINDOWS /D _WINDOWS /D UNICODE /D _UNICODE
LIBS=
INCLUDES=
TARGET=ClipWatcher.exe
OBJS=ClipWatcher.obj

all: $(TARGET)

test: $(TARGET)
	.\$(TARGET)

clean:
	-$(DEL) $(TARGET)
	-$(DEL) *.obj *.ilk *.pdb *.manifest

$(TARGET): $(OBJS)
	$(LINK) /manifest /out:$@ $** $(LIBS)
	$(MT) -manifest $@.manifest -outputresource:$@;1

ClipWatcher.obj: ClipWatcher.cpp

.cpp.obj:
	$(CL) $(CFLAGS) /Fo$@ /c $< $(DEFS) $(INCLUDES)

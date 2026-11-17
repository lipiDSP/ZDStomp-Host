NAME = ZDStompHost
FILES_DSP = ZoomMultiStompPlugin.cpp
FILES_UI = ZoomMultiStompGUI.cpp

BUILD_CXX_FLAGS += -std=c++17

LDFLAGS += -static -static-libgcc -static-libstdc++
include ../DPF/Makefile.plugins.mk

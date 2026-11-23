NAME = ZDStompHost
FILES_DSP = ZoomMultiStompPlugin.cpp
FILES_UI = ZoomMultiStompGUI.cpp

DPF_PATH ?= ../DPF
DPF_BUILD_DIR ?= ./build/$(NAME)
DPF_TARGET_DIR ?= ./bin

BUILD_CXX_FLAGS += -std=c++17

include $(DPF_PATH)/Makefile.plugins.mk

ifeq ($(WINDOWS),true)
LINK_FLAGS += -static -static-libgcc -static-libstdc++
endif

ifeq ($(MACOS),true)
all: vst3 au
else
all: vst3
endif

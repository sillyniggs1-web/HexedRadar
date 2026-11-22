LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := HexedRadar
LOCAL_SRC_FILES := HexedRadar.cpp
LOCAL_CPPFLAGS  := -O2 -std=c++17 -fvisibility=hidden -I$(LOCAL_PATH)/include
LOCAL_LDLIBS    := -llog
include $(BUILD_SHARED_LIBRARY)

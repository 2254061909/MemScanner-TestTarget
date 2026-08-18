LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := testtarget
LOCAL_SRC_FILES := native-targets.cpp
LOCAL_CPPFLAGS  := -std=c++17 -fexceptions
LOCAL_LDLIBS    :=
include $(BUILD_SHARED_LIBRARY)

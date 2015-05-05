LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := optional
#LOCAL_PRELINK_MODULE := false
LOCAL_C_INCLUDES += \
  $(LOCAL_PATH)/../utils/

LOCAL_SHARED_LIBRARIES := \
                          libcutils \
                          libhardware\
                          libdl\
                          libmfx_omx_core\
                          libgui\
                          libutils\
                          libui\
                          libm   

LOCAL_C_INCLUDES := \
    $(TARGET_OUT_HEADERS)/khronos/openmax \
    $(TOP)/frameworks/native/include/media/openmax \
    $(TOP)/frameworks/native/include/ui \
    $(TARGET_OUT_HEADERS)/libva\
    frameworks/native/include/media/hardware

LOCAL_32_BIT_ONLY := true


LOCAL_SRC_FILES := omxenc.cpp gralloc.cpp 
LOCAL_MODULE := omxenc 

include $(BUILD_EXECUTABLE)


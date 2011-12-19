# Copyright (C) 2008 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

ifeq ($(BOARD_SOC_CLASS),IMX5X)
ifeq ($(PREBUILT_FSL_IMX_GPU),false)

LOCAL_PATH := $(call my-dir)

# HAL module implemenation, not prelinked and stored in
# hw/<OVERLAY_HARDWARE_MODULE_ID>.<ro.product.board>.so
include $(CLEAR_VARS)
LOCAL_PRELINK_MODULE := false
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_C_INCLUDES += hardware/mx5x/libgralloc
LOCAL_C_INCLUDES += external/fsl_imx_gpu/driver/include/private/C2D
LOCAL_SHARED_LIBRARIES := liblog libcutils libc2d_z430

LOCAL_SRC_FILES := 	\
	copybit.cpp
	
LOCAL_MODULE := copybit.$(TARGET_BOARD_PLATFORM)
LOCAL_CFLAGS:= -DLOG_TAG=\"$(TARGET_BOARD_PLATFORM).copybit\" -D_LINUX
include $(BUILD_SHARED_LIBRARY)

endif
endif

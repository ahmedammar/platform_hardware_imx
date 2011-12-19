/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __OVERLAY_H__
#define __OVERLAY_H__

#include "overlay_utils.h"
#define DEFAULT_PMEM_ALIGN (4096)
#define PMEM_DEV "/dev/pmem_adsp"
#define MAX_SLOT 64

class PmemAllocator: public OverlayAllocator
{
public:
    PmemAllocator(int bufCount,int bufSize);
    virtual ~PmemAllocator();
    virtual int allocate(OVERLAY_BUFFER *overlay_buf, int size);
    virtual int deAllocate(OVERLAY_BUFFER *overlay_buf);
    virtual int getHeapID(){  return mFD;  }
private:
    int mFD;
    unsigned long mTotalSize;
    int mBufCount;
    int mBufSize;
    void *mVirBase;
    unsigned int mPhyBase;
    bool mSlotAllocated[MAX_SLOT];
};

#endif

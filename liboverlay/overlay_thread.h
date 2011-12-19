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
#ifndef __OVERLAY_THREAD_H__
#define __OVERLAY_THREAD_H__

#include <utils/threads.h>

class OverlayThread: public Thread {
    struct overlay_control_context_t *m_dev;
    struct v4l2_buffer mLatestQueuedBuf;
    ipu_lib_input_param_t mIPUInputParam;   
    ipu_lib_output_param_t mIPUOutputParam; 
    ipu_lib_handle_t            mIPUHandle;
    int mIPURet;
    public:
    OverlayThread(struct overlay_control_context_t *dev)
        : Thread(false),m_dev(dev){
        memset(&mIPUInputParam,0,sizeof(mIPUInputParam));
        memset(&mIPUOutputParam,0,sizeof(mIPUOutputParam));
        memset(&mIPUHandle,0,sizeof(mIPUHandle));
        memset(&mLatestQueuedBuf,0,sizeof(mLatestQueuedBuf));
    }

    virtual void onFirstRef() {
        OVERLAY_LOG_FUNC;
        //run("OverlayThread", PRIORITY_URGENT_DISPLAY);
    }
    virtual bool threadLoop() {
        int index = 0;
        overlay_object *overlayObj0;
        overlay_object *overlayObj1;
        overlay_data_shared_t *dataShared0;
        overlay_data_shared_t *dataShared1;
        unsigned int overlay_buf0;
        unsigned int overlay_buf1;
        bool outchange0;
        bool outchange1;
        WIN_REGION overlay0_outregion;
        WIN_REGION overlay1_outregion;
        int rotation0;
        int rotation1;
        int crop_x0 = 0,crop_y0 = 0,crop_w0 = 0,crop_h0 = 0;
        int crop_x1 = 0,crop_y1 = 0,crop_w1 = 0,crop_h1 = 0;
        while(m_dev&&(m_dev->overlay_running)) {
            OVERLAY_LOG_RUNTIME("Overlay thread running pid %d tid %d", getpid(),gettid());

            //Wait for semphore for overlay instance buffer queueing
            sem_wait(&m_dev->control_shared->overlay_sem);
            OVERLAY_LOG_RUNTIME("Get overlay semphore here pid %d tid %d",getpid(),gettid());
            overlayObj0 = NULL;
            overlayObj1 = NULL;
            dataShared0 = NULL;
            dataShared1 = NULL;
            overlay_buf0 = NULL;
            overlay_buf1 = NULL;
            outchange0 = false;
            outchange1 = false;
            memset(&overlay0_outregion, 0, sizeof(overlay0_outregion));
            memset(&overlay1_outregion, 0, sizeof(overlay1_outregion));

            //Check current active overlay instance
            pthread_mutex_lock(&m_dev->control_lock);
    
            if(m_dev->overlay_number >= 1) {
                for(index= 0;index < MAX_OVERLAY_INSTANCES;index++) {
                    if(m_dev->overlay_instance_valid[index]) {
                        if(!overlayObj0) {
                            overlayObj0 = m_dev->overlay_intances[index];
                        }
                        //For those small zorder, it should be drawn firstly
                        else if(m_dev->overlay_intances[index]->zorder < overlayObj0->zorder){
                            overlayObj1 = overlayObj0;
                            overlayObj0 = m_dev->overlay_intances[index];
                        }
                        else{
                            overlayObj1 = m_dev->overlay_intances[index];
                        }
                    }
                }
            }

            pthread_mutex_unlock(&m_dev->control_lock);            
               

            if(overlayObj0) {
                dataShared0 = overlayObj0->mDataShared;
                OVERLAY_LOG_RUNTIME("Process obj 0 instance_id %d, dataShared0 0x%x",
                                    dataShared0->instance_id,dataShared0);
                pthread_mutex_lock(&dataShared0->obj_lock);
                //Fetch one buffer from each overlay instance buffer queue
                OVERLAY_LOG_RUNTIME("queued_count %d,queued_head %d",
                     dataShared0->queued_count,dataShared0->queued_head);
                if(dataShared0->queued_count > 0) 
                {
                    //fetch the head buffer in queued buffers
                    overlay_buf0 = dataShared0->queued_bufs[dataShared0->queued_head];
                    OVERLAY_LOG_RUNTIME("id %d Get queue buffer for Overlay Instance 0: 0x%x queued_count %d",
                         dataShared0->instance_id,overlay_buf0,dataShared0->queued_count);
                    dataShared0->queued_bufs[dataShared0->queued_head] = 0;
                    dataShared0->queued_head ++;
                    dataShared0->queued_head = dataShared0->queued_head%MAX_OVERLAY_BUFFER_NUM;
                    dataShared0->queued_count --;
                }       

                //Check whether output area and zorder changing occure, so 
                //to paint v4l2 buffer or setting fb0/1's local 
                //alpha buffer                 
                outchange0 = overlayObj0->out_changed;
                if(overlay_buf0) {
                    overlayObj0->out_changed = 0;
                }
                overlay0_outregion.left = overlayObj0->outX;
                overlay0_outregion.right = overlayObj0->outX+overlayObj0->outW;
                overlay0_outregion.top = overlayObj0->outY;
                overlay0_outregion.bottom = overlayObj0->outY+overlayObj0->outH;
                rotation0 = overlayObj0->rotation;
                crop_x0 = dataShared0->crop_x;
                crop_y0 = dataShared0->crop_y;  
                crop_w0 = dataShared0->crop_w;
                crop_h0 = dataShared0->crop_h;
                pthread_mutex_unlock(&dataShared0->obj_lock); 
            }

            if(overlayObj1) {
                dataShared1 = overlayObj1->mDataShared;
                OVERLAY_LOG_RUNTIME("Process obj 0 instance_id %d dataShared0 0x%x",
                                    dataShared0->instance_id,dataShared0);
                pthread_mutex_lock(&dataShared1->obj_lock);
                //Fetch one buffer from each overlay instance buffer queue
                if(dataShared1->queued_count > 0) 
                {
                    //fetch the head buffer in queued buffers
                    overlay_buf1 = dataShared1->queued_bufs[dataShared1->queued_head];
                    OVERLAY_LOG_RUNTIME("Id %d Get queue buffer for Overlay Instance 1: 0x%x queued_count %d",
                         dataShared1->instance_id,overlay_buf1,dataShared1->queued_count);
                    dataShared1->queued_bufs[dataShared1->queued_head] = 0;
                    dataShared1->queued_head ++;
                    dataShared1->queued_head = dataShared1->queued_head%MAX_OVERLAY_BUFFER_NUM;
                    dataShared1->queued_count --;
                }       

                //Check whether output area and zorder changing occure, so 
                //to paint v4l2 buffer or setting fb0/1's local 
                //alpha buffer                 
                outchange1 = overlayObj1->out_changed;
                if(overlay_buf1) {
                    overlayObj1->out_changed = 0;
                }
                overlay1_outregion.left = overlayObj1->outX;
                overlay1_outregion.right = overlayObj1->outX+overlayObj1->outW;
                overlay1_outregion.top = overlayObj1->outY;
                overlay1_outregion.bottom = overlayObj1->outY+overlayObj1->outH;
                rotation1 = overlayObj1->rotation;
                crop_x1 = dataShared1->crop_x;
                crop_y1 = dataShared1->crop_y;  
                crop_w1 = dataShared1->crop_w;
                crop_h1 = dataShared1->crop_h;
                pthread_mutex_unlock(&dataShared1->obj_lock); 
            }

            if((!overlay_buf0)&&(!overlay_buf1)) {
                OVERLAY_LOG_RUNTIME("Nothing to do in overlay mixer thread!");
                //It is just a loop function in the loopless of this thread
                //So make a break here, and it will come back later
                continue;
            }
            if((overlay_buf0)&&(overlay_buf1)) {
                OVERLAY_LOG_RUNTIME("Two instance mixer needed");
            } 
                      
            //Check whether refill the origin area to black
            if(outchange0||outchange1) {
                OVERLAY_LOG_RUNTIME("Mixer thread refill the origin area to black");
            }

            //Check whether need copy back the latest frame to current frame
            //If only buf0  is available, copy back the overlay1's area in v4l latest buffer after show buf0
            //to v4l current buffer
            //If only buf1  is available, copy back the overlay1's area in v4l latest buffer before show buf1
            //to v4l current buffer

            //Dequeue a V4L2 Buffer
            struct v4l2_buffer *pV4LBuf;
            struct v4l2_buffer v4lbuf;
            memset(&v4lbuf, 0, sizeof(v4l2_buffer));
            if(m_dev->video_frames < m_dev->v4l_bufcount) {
                pV4LBuf = &m_dev->v4l_buffers[m_dev->video_frames];
            }
            else{
                v4lbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
                v4lbuf.memory = V4L2_MEMORY_MMAP;
                if(ioctl(m_dev->v4l_id, VIDIOC_DQBUF, &v4lbuf) < 0){
                    OVERLAY_LOG_ERR("Error!Cannot DQBUF a buffer from v4l");

                    //stream off it,so to make it recover
                    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
                    ioctl(m_dev->v4l_id, VIDIOC_STREAMOFF, &type);
                    m_dev->stream_on = false;
                    m_dev->video_frames = 0;
                    //reset latest queued buffer, since all buffer will start as init
                    memset(&mLatestQueuedBuf,0,sizeof(mLatestQueuedBuf));
                    goto free_buf_exit;
                }
                pV4LBuf = &v4lbuf;
            }


            OVERLAY_LOG_RUNTIME("DQBUF from v4l 0x%x:index %d, vir 0x%x, phy 0x%x, len %d",
                                pV4LBuf,pV4LBuf->index,m_dev->v4lbuf_addr[pV4LBuf->index],
                                pV4LBuf->m.offset,pV4LBuf->length);   
                      
            //Copyback before ipu update when two active instance, and obj0 has no buffer to update
            //obj1 has buffer to update
            if((overlayObj0)&&(overlayObj1)&&
               (!overlay_buf0)&&(overlay_buf1)&&
               (dataShared0->buf_showed))
            {
                OVERLAY_LOG_RUNTIME("Copyback before ipu update");
                if((mLatestQueuedBuf.m.offset)&&
                   (mLatestQueuedBuf.m.offset != pV4LBuf->m.offset)) {
                    //Copy back the region of obj0 to the current v4l buffer
                    //Setting input format
                    mIPUInputParam.width = m_dev->xres;
                    mIPUInputParam.height = m_dev->yres;

                    mIPUInputParam.input_crop_win.pos.x = overlay0_outregion.left;
                    mIPUInputParam.input_crop_win.pos.y = overlay0_outregion.top;  
                    mIPUInputParam.input_crop_win.win_w = overlay0_outregion.right - overlay0_outregion.left;
                    mIPUInputParam.input_crop_win.win_h = overlay0_outregion.bottom - overlay0_outregion.top;
                    mIPUInputParam.fmt = m_dev->outpixelformat;
                    mIPUInputParam.user_def_paddr[0] = mLatestQueuedBuf.m.offset;
    
                    //Setting output format
                    //Should align with v4l
                    mIPUOutputParam.fmt = m_dev->outpixelformat;
                    mIPUOutputParam.width = m_dev->xres;
                    mIPUOutputParam.height = m_dev->yres;   
                    mIPUOutputParam.show_to_fb = 0;
                    //Output param should be same as input, since no resize,crop
                    mIPUOutputParam.output_win.pos.x = mIPUInputParam.input_crop_win.pos.x;
                    mIPUOutputParam.output_win.pos.y = mIPUInputParam.input_crop_win.pos.y;
                    mIPUOutputParam.output_win.win_w = mIPUInputParam.input_crop_win.win_w;
                    mIPUOutputParam.output_win.win_h = mIPUInputParam.input_crop_win.win_h;
                    mIPUOutputParam.rot = 0;
                    mIPUOutputParam.user_def_paddr[0] = pV4LBuf->m.offset;
                    OVERLAY_LOG_RUNTIME("Copyback(before) Output param: width %d,height %d, pos.x %d, pos.y %d,win_w %d,win_h %d,rot %d",
                          mIPUOutputParam.width,
                          mIPUOutputParam.height,
                          mIPUOutputParam.output_win.pos.x,
                          mIPUOutputParam.output_win.pos.y,
                          mIPUOutputParam.output_win.win_w,
                          mIPUOutputParam.output_win.win_h,
                          mIPUOutputParam.rot);
                                     
                    OVERLAY_LOG_RUNTIME("Copyback(before) Input param: width %d, height %d, fmt %d, crop_win pos x %d, crop_win pos y %d, crop_win win_w %d,crop_win win_h %d",
                                     mIPUInputParam.width,
                                     mIPUInputParam.height,
                                     mIPUInputParam.fmt,
                                     mIPUInputParam.input_crop_win.pos.x,
                                     mIPUInputParam.input_crop_win.pos.y,
                                     mIPUInputParam.input_crop_win.win_w,
                                     mIPUInputParam.input_crop_win.win_h);     
    
                    mIPURet =  mxc_ipu_lib_task_init(&mIPUInputParam,NULL,&mIPUOutputParam,NULL,OP_NORMAL_MODE|TASK_PP_MODE,&mIPUHandle);
                    if (mIPURet < 0) {
                       OVERLAY_LOG_ERR("Error!Copyback(before) mxc_ipu_lib_task_init failed mIPURet %d!",mIPURet);
                       goto queue_buf_exit;
                    }  
                    OVERLAY_LOG_RUNTIME("Copyback(before) mxc_ipu_lib_task_init success");
                    mIPURet = mxc_ipu_lib_task_buf_update(&mIPUHandle,overlay_buf0,pV4LBuf->m.offset,NULL,NULL,NULL);
                    if (mIPURet < 0) {
                          OVERLAY_LOG_ERR("Error!Copyback(before) mxc_ipu_lib_task_buf_update failed mIPURet %d!",mIPURet);
                          mxc_ipu_lib_task_uninit(&mIPUHandle);
                          memset(&mIPUHandle, 0, sizeof(ipu_lib_handle_t));
                          goto queue_buf_exit;
                    }
                    OVERLAY_LOG_RUNTIME("Copyback(before) mxc_ipu_lib_task_buf_update success");
                    mxc_ipu_lib_task_uninit(&mIPUHandle);
                    memset(&mIPUHandle, 0, sizeof(ipu_lib_handle_t));
                }
                else{
                    OVERLAY_LOG_ERR("Error!Cannot Copyback before ipu update last buf 0x%x,curr buf 0x%x",
                                    mLatestQueuedBuf.m.offset,
                                    pV4LBuf->m.offset);
                }
            }

            //Mixer the first buffer from overlay instance0 to V4L2 Buffer
            if(overlay_buf0) {
                //Setting input format
                mIPUInputParam.width = overlayObj0->mHandle.width;
                mIPUInputParam.height = overlayObj0->mHandle.height;
                mIPUInputParam.input_crop_win.pos.x = crop_x0;
                mIPUInputParam.input_crop_win.pos.y = crop_y0;  
                mIPUInputParam.input_crop_win.win_w = crop_w0;
                mIPUInputParam.input_crop_win.win_h = crop_h0;

                if(overlayObj0->mHandle.format == PIXEL_FORMAT_YCbCr_420_SP) {
                    mIPUInputParam.fmt = v4l2_fourcc('I', '4', '2', '0');
                }
                else if(overlayObj0->mHandle.format == PIXEL_FORMAT_YCbCr_420_I) {
                    mIPUInputParam.fmt = v4l2_fourcc('N', 'Y', '1', '2');
                }
                else if(overlayObj0->mHandle.format == PIXEL_FORMAT_RGB_565) {
                    mIPUInputParam.fmt = v4l2_fourcc('R', 'G', 'B', 'P');
                }else{
                    OVERLAY_LOG_ERR("Error!Not supported input format %d",overlayObj0->mHandle.format);
                    goto queue_buf_exit;
                }
                mIPUInputParam.user_def_paddr[0] = overlay_buf0;

                //Setting output format
                //Should align with v4l
                mIPUOutputParam.fmt = m_dev->outpixelformat;
                mIPUOutputParam.width = m_dev->xres;
                mIPUOutputParam.height = m_dev->yres;   
                mIPUOutputParam.show_to_fb = 0;
                mIPUOutputParam.output_win.pos.x = overlayObj0->outX;
                mIPUOutputParam.output_win.pos.y = overlayObj0->outY;
                mIPUOutputParam.output_win.win_w = overlayObj0->outW;
                mIPUOutputParam.output_win.win_h = overlayObj0->outH;
                mIPUOutputParam.rot = overlayObj0->rotation;
                mIPUOutputParam.user_def_paddr[0] = pV4LBuf->m.offset;
                OVERLAY_LOG_RUNTIME("Obj0 Output param: width %d,height %d, pos.x %d, pos.y %d,win_w %d,win_h %d,rot %d",
                      mIPUOutputParam.width,
                      mIPUOutputParam.height,
                      mIPUOutputParam.output_win.pos.x,
                      mIPUOutputParam.output_win.pos.y,
                      mIPUOutputParam.output_win.win_w,
                      mIPUOutputParam.output_win.win_h,
                      mIPUOutputParam.rot);
                                 
                OVERLAY_LOG_RUNTIME("Obj0 Input param: width %d, height %d, fmt %d, crop_win pos x %d, crop_win pos y %d, crop_win win_w %d,crop_win win_h %d",
                                 mIPUInputParam.width,
                                 mIPUInputParam.height,
                                 mIPUInputParam.fmt,
                                 mIPUInputParam.input_crop_win.pos.x,
                                 mIPUInputParam.input_crop_win.pos.y,
                                 mIPUInputParam.input_crop_win.win_w,
                                 mIPUInputParam.input_crop_win.win_h);     

                mIPURet =  mxc_ipu_lib_task_init(&mIPUInputParam,NULL,&mIPUOutputParam,NULL,OP_NORMAL_MODE|TASK_PP_MODE,&mIPUHandle);
                if (mIPURet < 0) {
                   OVERLAY_LOG_ERR("Error!Obj0 mxc_ipu_lib_task_init failed mIPURet %d!",mIPURet);
                   goto queue_buf_exit;
                }  
                OVERLAY_LOG_RUNTIME("Obj0 mxc_ipu_lib_task_init success");
                mIPURet = mxc_ipu_lib_task_buf_update(&mIPUHandle,overlay_buf0,pV4LBuf->m.offset,NULL,NULL,NULL);
                if (mIPURet < 0) {
                      OVERLAY_LOG_ERR("Error!Obj0 mxc_ipu_lib_task_buf_update failed mIPURet %d!",mIPURet);
                      mxc_ipu_lib_task_uninit(&mIPUHandle);
                      memset(&mIPUHandle, 0, sizeof(ipu_lib_handle_t));
                      goto queue_buf_exit;
                }
                OVERLAY_LOG_RUNTIME("Obj0 mxc_ipu_lib_task_buf_update success");
                mxc_ipu_lib_task_uninit(&mIPUHandle);
                memset(&mIPUHandle, 0, sizeof(ipu_lib_handle_t));
            }

            //Check whether we need to do another mixer, based on
            //buffers in overlay instance1's buffer queue
            if(overlay_buf1) {
                //Setting input format
                mIPUInputParam.width = overlayObj1->mHandle.width;
                mIPUInputParam.height = overlayObj1->mHandle.height;
                mIPUInputParam.input_crop_win.pos.x = crop_x1;
                mIPUInputParam.input_crop_win.pos.y = crop_y1;  
                mIPUInputParam.input_crop_win.win_w = crop_w1;
                mIPUInputParam.input_crop_win.win_h = crop_h1;


                if(overlayObj1->mHandle.format == PIXEL_FORMAT_YCbCr_420_SP) {
                    mIPUInputParam.fmt = v4l2_fourcc('I', '4', '2', '0');
                }
                else if(overlayObj1->mHandle.format == PIXEL_FORMAT_YCbCr_420_I) {
                    mIPUInputParam.fmt = v4l2_fourcc('N', 'Y', '1', '2');
                }
                else if(overlayObj0->mHandle.format == PIXEL_FORMAT_RGB_565) {
                    mIPUInputParam.fmt = v4l2_fourcc('R', 'G', 'B', 'P');
                }else{
                    OVERLAY_LOG_ERR("Error!Obj1 Not supported input format %d",overlayObj1->mHandle.format);
                    goto queue_buf_exit;
                }
                mIPUInputParam.user_def_paddr[0] = overlay_buf1;

                //Setting output format
                mIPUOutputParam.fmt = v4l2_fourcc('U', 'Y', 'V', 'Y');
                mIPUOutputParam.width = m_dev->xres;
                mIPUOutputParam.height = m_dev->yres;   
                mIPUOutputParam.show_to_fb = 0;
                mIPUOutputParam.output_win.pos.x = overlayObj1->outX;
                mIPUOutputParam.output_win.pos.y = overlayObj1->outY;
                mIPUOutputParam.output_win.win_w = overlayObj1->outW;
                mIPUOutputParam.output_win.win_h = overlayObj1->outH;
                mIPUOutputParam.rot = overlayObj1->rotation;
                mIPUOutputParam.user_def_paddr[0] = pV4LBuf->m.offset;
                OVERLAY_LOG_RUNTIME("Obj1 Output param: width %d,height %d, pos.x %d, pos.y %d,win_w %d,win_h %d,rot %d",
                      mIPUOutputParam.width,
                      mIPUOutputParam.height,
                      mIPUOutputParam.output_win.pos.x,
                      mIPUOutputParam.output_win.pos.y,
                      mIPUOutputParam.output_win.win_w,
                      mIPUOutputParam.output_win.win_h,
                      mIPUOutputParam.rot);
                                 
                OVERLAY_LOG_RUNTIME("Obj1 Input param:width %d,height %d,fmt %d,crop_win pos x %d,crop_win pos y %d,crop_win win_w %d,crop_win win_h %d",
                                 mIPUInputParam.width,
                                 mIPUInputParam.height,
                                 mIPUInputParam.fmt,
                                 mIPUInputParam.input_crop_win.pos.x,
                                 mIPUInputParam.input_crop_win.pos.y,
                                 mIPUInputParam.input_crop_win.win_w,
                                 mIPUInputParam.input_crop_win.win_h);     

                mIPURet =  mxc_ipu_lib_task_init(&mIPUInputParam,NULL,&mIPUOutputParam,NULL,OP_NORMAL_MODE|TASK_PP_MODE,&mIPUHandle);
                if (mIPURet < 0) {
                   OVERLAY_LOG_ERR("Error!Obj1 mxc_ipu_lib_task_init failed mIPURet %d!",mIPURet);
                   goto queue_buf_exit;
                }  
                OVERLAY_LOG_RUNTIME("Obj1 mxc_ipu_lib_task_init success");
                mIPURet = mxc_ipu_lib_task_buf_update(&mIPUHandle,overlay_buf1,pV4LBuf->m.offset,NULL,NULL,NULL);
                if (mIPURet < 0) {
                      OVERLAY_LOG_ERR("Error!Obj1 mxc_ipu_lib_task_buf_update failed mIPURet %d!",mIPURet);
                      mxc_ipu_lib_task_uninit(&mIPUHandle);
                      memset(&mIPUHandle, 0, sizeof(ipu_lib_handle_t));
                      goto queue_buf_exit;
                }
                OVERLAY_LOG_RUNTIME("Obj1 mxc_ipu_lib_task_buf_update success");
                mxc_ipu_lib_task_uninit(&mIPUHandle);
                memset(&mIPUHandle, 0, sizeof(ipu_lib_handle_t));
            }
        
            //Copyback after ipu update when two active instance, and obj0 has one buffer to update
            //obj0 has no buffer to update
            if((overlayObj0)&&(overlayObj1)&&
               (overlay_buf0)&&(!overlay_buf1)&&
               (dataShared1->buf_showed))
            {
                OVERLAY_LOG_RUNTIME("Copyback after ipu update");
                if((mLatestQueuedBuf.m.offset)&&
                   (mLatestQueuedBuf.m.offset != pV4LBuf->m.offset)) {
                    //Copy back the region of obj0 to the current v4l buffer
                    //Setting input format
                    mIPUInputParam.width = m_dev->xres;
                    mIPUInputParam.height = m_dev->yres;

                    mIPUInputParam.input_crop_win.pos.x = overlay1_outregion.left;
                    mIPUInputParam.input_crop_win.pos.y = overlay1_outregion.top;  
                    mIPUInputParam.input_crop_win.win_w = overlay1_outregion.right - overlay1_outregion.left;
                    mIPUInputParam.input_crop_win.win_h = overlay1_outregion.bottom - overlay1_outregion.top;
                    mIPUInputParam.fmt = m_dev->outpixelformat;
                    mIPUInputParam.user_def_paddr[0] = mLatestQueuedBuf.m.offset;
    
                    //Setting output format
                    //Should align with v4l
                    mIPUOutputParam.fmt = m_dev->outpixelformat;
                    mIPUOutputParam.width = m_dev->xres;
                    mIPUOutputParam.height = m_dev->yres;   
                    mIPUOutputParam.show_to_fb = 0;
                    //Output param should be same as input, since no resize,crop
                    mIPUOutputParam.output_win.pos.x = mIPUInputParam.input_crop_win.pos.x;
                    mIPUOutputParam.output_win.pos.y = mIPUInputParam.input_crop_win.pos.y;
                    mIPUOutputParam.output_win.win_w = mIPUInputParam.input_crop_win.win_w;
                    mIPUOutputParam.output_win.win_h = mIPUInputParam.input_crop_win.win_h;
                    mIPUOutputParam.rot = 0;
                    mIPUOutputParam.user_def_paddr[0] = pV4LBuf->m.offset;
                    OVERLAY_LOG_RUNTIME("Copyback(after) Output param: width %d,height %d, pos.x %d, pos.y %d,win_w %d,win_h %d,rot %d",
                          mIPUOutputParam.width,
                          mIPUOutputParam.height,
                          mIPUOutputParam.output_win.pos.x,
                          mIPUOutputParam.output_win.pos.y,
                          mIPUOutputParam.output_win.win_w,
                          mIPUOutputParam.output_win.win_h,
                          mIPUOutputParam.rot);
                                     
                    OVERLAY_LOG_RUNTIME("Copyback(after) Input param: width %d, height %d, fmt %d, crop_win pos x %d, crop_win pos y %d, crop_win win_w %d,crop_win win_h %d",
                                     mIPUInputParam.width,
                                     mIPUInputParam.height,
                                     mIPUInputParam.fmt,
                                     mIPUInputParam.input_crop_win.pos.x,
                                     mIPUInputParam.input_crop_win.pos.y,
                                     mIPUInputParam.input_crop_win.win_w,
                                     mIPUInputParam.input_crop_win.win_h);     
    
                    mIPURet =  mxc_ipu_lib_task_init(&mIPUInputParam,NULL,&mIPUOutputParam,NULL,OP_NORMAL_MODE|TASK_PP_MODE,&mIPUHandle);
                    if (mIPURet < 0) {
                       OVERLAY_LOG_ERR("Error!Copyback(after) mxc_ipu_lib_task_init failed mIPURet %d!",mIPURet);
                       goto queue_buf_exit;
                    }  
                    OVERLAY_LOG_RUNTIME("Copyback(after) mxc_ipu_lib_task_init success");
                    mIPURet = mxc_ipu_lib_task_buf_update(&mIPUHandle,overlay_buf0,pV4LBuf->m.offset,NULL,NULL,NULL);
                    if (mIPURet < 0) {
                          OVERLAY_LOG_ERR("Error!Copyback(after) mxc_ipu_lib_task_buf_update failed mIPURet %d!",mIPURet);
                          mxc_ipu_lib_task_uninit(&mIPUHandle);
                          memset(&mIPUHandle, 0, sizeof(ipu_lib_handle_t));
                          goto queue_buf_exit;
                    }
                    OVERLAY_LOG_RUNTIME("Copyback(after) mxc_ipu_lib_task_buf_update success");
                    mxc_ipu_lib_task_uninit(&mIPUHandle);
                    memset(&mIPUHandle, 0, sizeof(ipu_lib_handle_t));
                }
                else{
                    OVERLAY_LOG_ERR("Error!Cannot Copyback before ipu update last buf 0x%x,curr buf 0x%x",
                                    mLatestQueuedBuf.m.offset,
                                    pV4LBuf->m.offset);
                }
            }

queue_buf_exit:
            //Queue the mixed V4L2 Buffer for display
            gettimeofday(&pV4LBuf->timestamp, 0);
            if(ioctl(m_dev->v4l_id, VIDIOC_QBUF, pV4LBuf) < 0){
                OVERLAY_LOG_ERR("Error!Cannot QBUF a buffer from v4l");
                //reset latest queued buffer, since all buffer will start as init
                memset(&mLatestQueuedBuf,0,sizeof(mLatestQueuedBuf));
                goto free_buf_exit;
            }
            OVERLAY_LOG_RUNTIME("QBUF from v4l at frame %d:index %d, phy 0x%x at sec %d usec %d",
                                m_dev->video_frames,pV4LBuf->index,pV4LBuf->m.offset,
                                pV4LBuf->timestamp.tv_sec,pV4LBuf->timestamp.tv_usec); 

            //record the latest buffer we queued
            memcpy(&mLatestQueuedBuf,pV4LBuf,sizeof(mLatestQueuedBuf));
            
            //Only stream on after two frames queued 
            m_dev->video_frames++;
            if((m_dev->video_frames>=2)&&(!m_dev->stream_on)) {
                int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
                ioctl(m_dev->v4l_id, VIDIOC_STREAMON, &type);
                m_dev->stream_on = true;
                OVERLAY_LOG_INFO("V4L STREAMON NOW");
            }

free_buf_exit:
            //push back the buffer to overlay instance0 freequeue
            //signal instance condition if wait flag is true
            //reset wait flag
            //pthread_cond_signal(&cond);  
            if((overlayObj0)&&(overlay_buf0)) {
                pthread_mutex_lock(&dataShared0->obj_lock);
                dataShared0->buf_showed++;
                dataShared0->free_bufs[dataShared0->free_tail] = overlay_buf0;
                OVERLAY_LOG_RUNTIME("Id %d back buffer to free queue for Overlay Instance 0: 0x%x at %d free_count %d",
                     dataShared0->instance_id,overlay_buf0,dataShared0->free_tail,dataShared0->free_count+1);
                dataShared0->free_tail++;
                dataShared0->free_tail = dataShared0->free_tail%MAX_OVERLAY_BUFFER_NUM;
                dataShared0->free_count++;
                if(dataShared0->free_count > dataShared0->num_buffer) {
                    OVERLAY_LOG_ERR("Error!free_count %d is greater the total number %d",
                                    dataShared0->free_count,dataShared0->num_buffer);
                }

                if(dataShared0->wait_buf_flag) {
                    dataShared0->wait_buf_flag = 0;
                    OVERLAY_LOG_RUNTIME("Id %d Condition signal for Overlay Instance 0",dataShared0->instance_id);
                    pthread_cond_signal(&dataShared0->free_cond);
                }
                pthread_mutex_unlock(&dataShared0->obj_lock);
            }

            //push back the buffer of overlay instance1 freequeue
            //signal instance condition if wait flag is true
            //reset wait flag
            //pthread_cond_signal(&cond);  
            if((overlayObj1)&&(overlay_buf1)) {
                pthread_mutex_lock(&dataShared1->obj_lock);
                dataShared1->buf_showed++;
                dataShared1->free_bufs[dataShared1->free_tail] = overlay_buf1;
                dataShared1->free_tail++;
                dataShared1->free_tail = dataShared1->free_tail%MAX_OVERLAY_BUFFER_NUM;
                dataShared1->free_count++;
                OVERLAY_LOG_RUNTIME("Id %d back buffer to free queue for Overlay Instance 0: 0x%x free_count %d",
                     dataShared1->instance_id,overlay_buf1,dataShared1->free_count);

                if(dataShared1->wait_buf_flag) {
                    dataShared1->wait_buf_flag = 0;
                    OVERLAY_LOG_RUNTIME("Id %d Condition signal for Overlay Instance 1",
                                        dataShared1->instance_id);
                    pthread_cond_signal(&dataShared1->free_cond);
                }
                pthread_mutex_unlock(&dataShared1->obj_lock);
            }
            
        }
        // loop until we need to quit
        return true;
    }
};

#endif

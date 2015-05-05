#include <getopt.h>

#include "omxenc.h"
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/hardware/MetadataBufferType.h>

static OMX_STRING StrVideoEncoder;
static OMX_STRING StrVideoEncoderAVC = "OMX.Intel.hw_ve.h264";

static Mutex mLock;
static Condition mAsyncCompletion;

bool mNoOutputFlag = OMX_FALSE;
bool mRunning = OMX_FALSE;
static bool gNothread = OMX_FALSE;

pthread_t mThread;
static Mutex mMsgLock;
static Condition mMessageReceive;
android::List<omx_message> msgQueue;
static Mutex mEventLock;
static Condition mEventReceive;
android::List<omx_message> eventQueue;

int GetTickCount()
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL))
        return 0;
    return tv.tv_usec / 1000 + tv.tv_sec * 1000;
}

void PerfBegin(MYDATATYPE* pAppData)
{


    pAppData->nEncBegin = pAppData->nTc2 = GetTickCount();
}

void PerfEnd(MYDATATYPE* pAppData)
{
    pAppData->nEncEnd = pAppData->nTc2 = GetTickCount();
}

void DumpPerf(MYDATATYPE* pAppData)
{


    pAppData->nCurrentFrameIn = pAppData->nCurrentFrameIn - pAppData->number_of_rewind;


    printf("\n\n");
    printf("PERFORMANCE:          Number of frames encoded : %ldframes\n", pAppData->nCurrentFrameOut - 1);
    printf("PERFORMANCE:      Total size of frames encoded : %ldkb\n", pAppData->nWriteLen / 1000);
    printf("PERFORMANCE:                Total time elapsed : %dms\n", pAppData->nEncEnd - pAppData->nEncBegin);
    printf("PERFORMANCE:          Time upload data encoder : %dms\n", pAppData->nUpload);
    printf("PERFORMANCE:        Time spend at encoder side : %dms\n",
           pAppData->nEncEnd - pAppData->nEncBegin - pAppData->nUpload);
    printf("PERFORMANCE:   Resolution(%4dx%4d) performance : %ffps\n",
           pAppData->nWidth, pAppData->nHeight,
           (float)(pAppData->nCurrentFrameOut) / (float)(pAppData->nEncEnd - pAppData->nEncBegin) * 1000);
}

int YUV_Generator_Planar(int width, int height,
                         unsigned char *Y_start, int Y_pitch,
                         unsigned char *U_start, int U_pitch,
                         unsigned char *V_start, int V_pitch,
                         int UV_interleave
                        )
{
    static int row_shift = 0;
    int box_width = 8;

    row_shift++;
    if (row_shift == 16) row_shift = 0;

    V_start = U_start + 1;
    yuvgen_planar(width,  height,
                  Y_start,  Y_pitch,
                  U_start,  U_pitch,
                  V_start,  V_pitch,
                  VA_FOURCC_NV12,  box_width,  row_shift,
                  0);

    return 0;
}


int OMXENC_fill_data(OMX_BUFFERHEADERTYPE *pBuf, FILE *fIn, int bufferSize)
{
    int nRead = 0;
    MYDATATYPE *pApp = (MYDATATYPE*)pBuf->pAppPrivate;

    if (pApp->fIn) {
        nRead = fread(pBuf->pBuffer, 1, bufferSize, fIn);
        if (nRead == -1) {
            printf("Error Reading File!\n");
        }
        int nError = ferror(fIn);
        if (nError != 0) {
            printf("ERROR: reading file\n");
        }
        nError = feof(fIn);
        if (nError != 0) {
            printf("EOS of source file rewind it");
            clearerr(fIn);
            rewind(fIn);
            nRead = fread(pBuf->pBuffer, 1, bufferSize, fIn);
            pApp->number_of_rewind++;
        }
    } else {
        nRead = pApp->nWidth * pApp->nHeight * 3 / 2;
        YUV_Generator_Planar(pApp->nWidth, pApp->nHeight,
                             pBuf->pBuffer, pApp->nWidth,
                             pBuf->pBuffer + pApp->nWidth * pApp->nHeight,  pApp->nWidth,
                             pBuf->pBuffer + pApp->nWidth * pApp->nHeight,  pApp->nWidth,
                             1);
    }

    pBuf->nFilledLen = nRead;
    printf("%s,  buffer 0x%x, buffersize %d , length %d \n",
           __FUNCTION__,
           *((int*)pBuf->pBuffer),
           bufferSize, pBuf->nFilledLen);

    return nRead;
}

int OMXENC_fill_metadata(OMX_BUFFERHEADERTYPE *pBuf, int buffersize)
{
    int nRead = 0;
    int i;
    MYDATATYPE *pApp = (MYDATATYPE*)pBuf->pAppPrivate;
    int pitch_y = ((pApp->nWidth + 31) & (~31));

    if (pApp->memMode == MEM_MODE_GFXHANDLE) {
        if (!pApp->pUsrAddr[0]) {
            int32_t stride;
            void *vaddr[3];
            int width = (pApp->nWidth + 31) & (~31);
            int height = (pApp->nHeight + 15) & (~15);
            alloc_gralloc_buffer(pApp->nInputBufNum, width , height, pApp->handle_t);
            for (i = 0; i < pApp->nInputBufNum; i++) {
                pApp->pUsrAddr[i] = (uint8_t*) lockGraphicBuf(i);
                printf("===== user_pointer[%d] is 0x%x, handle_t[%d] 0x%x \n", i, pApp->pUsrAddr[i], i, pApp->handle_t[i]);
            }
        }
    } else {
        int size_of_buffer = pitch_y * pApp->nHeight * 3 / 2;
        if (!pApp->pUsrAddr[0])
            for (i = 0; i < pApp->nInputBufNum; i++) {
                pApp->pUsrAddr[i] = (uint8_t *)malloc(size_of_buffer + 4095);
                pApp->pUsrAddr[i] = (uint8_t *)((((unsigned long)pApp->pUsrAddr[i] + 4095) / 4096 * 4096));
                printf("=====temp_user_pointer[%d] is %x \n", i, pApp->pUsrAddr[i]);
            }
    }

    if (pApp->fIn) {
        void *UV_start;
        void *Y_start;

        int row;
        printf("start to read Y <==== \n");
        for (row = 0; row < pApp->nHeight; row++) {
            unsigned char *row_start = pApp->pUsrAddr[pApp->index_metadata] + row * pitch_y;
            nRead += fread(row_start, 1, pApp->nWidth, pApp->fIn);
        }
        printf("start to read UV <==== \n");
        UV_start = pApp->pUsrAddr[pApp->index_metadata] + pApp->nHeight * pitch_y;
        for (row = 0; row < pApp->nHeight / 2; row++) {
            unsigned char *row_start = (unsigned char *)UV_start + row * pitch_y;
            nRead += fread(row_start, 1, pApp->nWidth, pApp->fIn);
        }
    } else {
        YUV_Generator_Planar(pitch_y, pApp->nHeight,
                             pApp->pUsrAddr[pApp->index_metadata], pitch_y,
                             pApp->pUsrAddr[pApp->index_metadata] + pitch_y * pApp->nHeight,  pitch_y,
                             pApp->pUsrAddr[pApp->index_metadata] + pitch_y * pApp->nHeight,  pitch_y,
                             1);
    }

    uint32_t size =4 + sizeof(buffer_handle_t);
    char *buffer = new char[size];
    // char *data = (char *)(*buffer).data();
    OMX_U32 type =  android::kMetadataBufferTypeGrallocSource;
    memcpy(buffer, &type, 4);
    memcpy(buffer + 4, &pApp->handle_t[pApp->index_metadata], sizeof(buffer_handle_t));
    memcpy(pBuf->pBuffer,  buffer, size);
    nRead = size;
    free(buffer);

    pApp->index_metadata++;
    pApp->index_metadata = pApp->index_metadata % pApp->nInputBufNum;

    if (pApp->fIn && feof(pApp->fIn)) {
        printf("EOS of source file rewind it");
        clearerr(pApp->fIn);
        rewind(pApp->fIn);
        nRead = 0;
        pApp->number_of_rewind++;
    }

    return nRead;
}

OMX_ERRORTYPE OMXENC_HandleError(MYDATATYPE* pAppData, OMX_ERRORTYPE eError)
{
    OMXENC_PRINT(">> %s\n", __FUNCTION__);
    OMX_ERRORTYPE eErrorHandleError = OMX_ErrorNone;
    OMX_HANDLETYPE pHandle = pAppData->pHandle;
    OMX_U32 nCounter;


    switch (pAppData->eCurrentState) {
    case OMX_StateIdle:
    case OMX_StateExecuting:
        OMXENC_FreeBuffer(pAppData);
    case OMX_StateLoaded:
        eError = OMX_SendCommand(pHandle, OMX_CommandStateSet, OMX_StateLoaded, NULL);
        OMXENC_CHECK_EXIT(eError, "Error at OMX_SendCommand function");
        OMXENC_DeInit(pAppData);
    default:
        ;
    }

EXIT:
    OMXENC_PRINT(">> %s\n", __FUNCTION__);
    return eErrorHandleError;
}

//message thread
OMX_ERRORTYPE OMXENC_EventHandler(OMX_PTR pData,
                                  OMX_EVENTTYPE eEvent,
                                  OMX_U32 nData1,
                                  OMX_U32 nData2)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    MYDATATYPE *pAppData = (MYDATATYPE*) pData;

    OMXENC_PRINT("EventHandler: event %d nData1 %d nData2 : 0x%x\n", eEvent, nData1, nData2);

    switch (eEvent) {
    case OMX_EventCmdComplete:
        switch (nData1) {
        case OMX_CommandStateSet:
        {
            Mutex::Autolock autoLock(mLock);
            pAppData->eCurrentState = (OMX_STATETYPE)nData2;
            mAsyncCompletion.signal();
        }
        break;
        case OMX_CommandFlush:
            OMXENC_PRINT("OMX_CommandFlush port %ld  \n", nData2);
            if (pAppData->eCurrentState == OMX_StateExecuting && nData2 == OMX_DirOutput ) {
                Mutex::Autolock autoLock(mLock);
                pAppData->eCurrentState = (OMX_STATETYPE)OMX_StateIdle;
                mAsyncCompletion.signal();
            }
            break;
        case OMX_CommandPortDisable:
            OMXENC_PRINT("%s:%d  OMX_CommandPortDisable event handle \n", __FUNCTION__, __LINE__);
            if(OMX_DirOutput == nData2)
            {
                if(!gNothread) {
                    eError= OMX_SendCommand(pAppData->pHandle,  OMX_CommandPortEnable, nData2,NULL);
                    OMXENC_allocateOnPort(pAppData, pAppData->pOutPortDef,pAppData->pOutBuff);

                    for (uint32_t nCounter = 0; nCounter < pAppData->pOutPortDef->nBufferCountActual; nCounter++) {
                        printf("FillThisBuffer output buffer %p \n", pAppData->pOutBuff[nCounter]);
                        eError = OMX_FillThisBuffer(pAppData->pComponent, pAppData->pOutBuff[nCounter]);
                        OMXENC_CHECK_EXIT(eError, "Error in FillThisBuffer");
                    }
                }
                if(gNothread)
                {
                    Mutex::Autolock autoLock(mLock);
                    pAppData->nPortReEnable = OMX_TRUE;
                    mAsyncCompletion.signal();
                }
            }
            break;
        case  OMX_CommandPortEnable:
            OMXENC_PRINT("OMX_CommandPortEnable port %ld  \n", nData2);
            {
                Mutex::Autolock autoLock(mLock);
                pAppData->eCurrentState = (OMX_STATETYPE)OMX_StateExecuting;
                mAsyncCompletion.signal();
            }
            break;
        case OMX_CommandMarkBuffer:
        default :
            ;
        }
        break;
    case OMX_EventError: {
        OMXENC_PRINT("\n%s::%d, nData1: %x, nData2 : %x\n", __FUNCTION__, __LINE__,nData1, nData2);

        //      OMXENC_Stop(pAppData);
        //      eError = OMX_SendCommand(pAppData->pHandle,
        //             OMX_CommandStateSet, OMX_StateLoaded, NULL);
        //  OMXENC_CHECK_EXIT(eError, "Error at OMX_SendCommand function");

//        eError =  OMXENC_DeInit(pAppData);
//        OMXENC_CHECK_EXIT(eError, "Error at OMX_Deinit function");
//        exit(0);
        break;
    }
    case OMX_EventBufferFlag:
        printf("Detect EOS at EmptyThisBuffer function \n");
        mNoOutputFlag = OMX_TRUE;
        pAppData->nFlags = OMX_BUFFERFLAG_EOS;
        mAsyncCompletion.signal();
        break;
    case OMX_EventPortSettingsChanged:
        if (nData2 == 0 || nData1 == OMX_DirOutput) {
            if(!gNothread)
            {
                eError= OMX_SendCommand(pAppData->pHandle, OMX_CommandPortDisable, nData1,NULL);

                for (uint32_t nCounter = 0; nCounter < pAppData->pOutPortDef->nBufferCountActual; nCounter++) {
                    eError = OMX_FreeBuffer(pAppData->pComponent,1, pAppData->pOutBuff[nCounter]);
                    OMXENC_CHECK_EXIT(eError, "Error in freeBuffer");
                }
            }

            Mutex::Autolock autoLock(mLock);
            if(gNothread)	{
                OMXENC_PRINT("need re-enable output port\n");
                pAppData->nPortReEnable = OMX_TRUE;
            }
            pAppData->eCurrentState = OMX_StateIdle;
            mAsyncCompletion.signal();
        }
        break;
    default:
        OMXENC_CHECK_ERROR(OMX_ErrorUndefined, "Error at EmptyThisBuffer function");
        break;
    }

EXIT:
    return eError;
}

//message thread
OMX_ERRORTYPE OMXENC_FillBufferDone(OMX_PTR pData,
                                    OMX_BUFFERHEADERTYPE* pBuffer)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    MYDATATYPE* pAppData = (MYDATATYPE*)pData;
    OMX_HANDLETYPE pHandle = pAppData->pHandle;

    unsigned int vp8_frame_length = 0;

    if (mNoOutputFlag == OMX_TRUE)
        return OMX_ErrorNone;

    if (pAppData->eCurrentState != OMX_StateExecuting) {
        printf("%4d error state.\n", __LINE__);
        goto EXIT;
    }

    printf("pBuffer %p \n",
           pBuffer);
    if( !pBuffer->nFilledLen)
        return OMX_ErrorNone;

    if( pBuffer->nFilledLen==0)
        return OMX_ErrorNone;


    printf("FILLBUFFERDONE %p size %d value %x, nCurrentFrameIn %d, nCurrentFrameOut %d (%4d)\n",
           pBuffer,
           pBuffer->nFilledLen,
           *(int*)pBuffer->pBuffer,
           pAppData->nCurrentFrameIn,
           pAppData->nCurrentFrameOut,
           __LINE__);

    if( pBuffer->nFilledLen==0)
        return OMX_ErrorNone;

    /* check is it is the last buffer */
    if (pBuffer->nFlags & OMX_BUFFERFLAG_EOS)
        printf("Last OutBuffer\n");

    pAppData->nCurrentFrameOut++;

    /*App sends last buffer as null buffer, so buffer with EOS contains only garbage*/
    if (pBuffer->nFilledLen) {
        pAppData->nWriteLen += fwrite(pBuffer->pBuffer + pBuffer->nOffset,
                                      1, pBuffer->nFilledLen, pAppData->fOut);

        pAppData->nBitrateSize += pBuffer->nFilledLen;
        pAppData->nSavedFrameNum++;
        if (ferror(pAppData->fOut))
            printf("ERROR: writing to file=\n");

        if (fflush(pAppData->fOut))
            printf("ERROR: flushing file -----\n");

        if (pAppData->nSavedFrameNum % pAppData->nFramerate== 0) {
            printf(".");
            fflush(stdout);
        }


        if (pAppData->bShowBitrateRealTime) {
            if ((pAppData->nSavedFrameNum) % pAppData->nFramerate == 0) {
                printf("PERFORMANCE:       frame(%d - %d), bitrate : %d kbps\n",
                       pAppData->nSavedFrameNum - pAppData->nFramerate,
                       pAppData->nSavedFrameNum,
                       pAppData->nBitrateSize * 8 / 1000);
                pAppData->nBitrateSize = 0;
            }
        }
    }

#if 0
    if (pAppData->eCompressionFormat == OMX_VIDEO_CodingVP8)
        eError = OMXENC_DynamicChangeVPX(pAppData);
    else
        eError = OMXENC_DynamicChange(pAppData);
    OMXENC_CHECK_ERROR(eError, "Error at OMXENC_DynamicChange function");
#endif

    pBuffer->nFilledLen = 0;

    printf("refill this buffer\n");
    eError = OMX_FillThisBuffer(pAppData->pComponent, pBuffer);
    OMXENC_CHECK_ERROR(eError, "Error at FillThisBuffer function");

EXIT:
    return eError;
}

//message thread
OMX_ERRORTYPE OMXENC_EmptyBufferDone(OMX_PTR pData,
                                     OMX_BUFFERHEADERTYPE* pBuffer)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    MYDATATYPE* pAppData = (MYDATATYPE*)pData;
    OMX_HANDLETYPE pHandle = pAppData->pHandle;

    if (mNoOutputFlag == OMX_TRUE)
        return OMX_ErrorNone;

    printf("EMPTYBUFFERDONE %p size %d value %x, nCurrentFrameIn %d, nCurrentFrameOut %d (%4d) \n",
           pBuffer, pBuffer->nFilledLen,
           *(int*)pBuffer->pBuffer,
           pAppData->nCurrentFrameIn,
           pAppData->nCurrentFrameOut,
           __LINE__);

    if (pAppData->eCurrentState != OMX_StateExecuting) {
        printf("error state (%4d)", __LINE__);
        goto EXIT;
    }


    if (pAppData->nCurrentFrameIn == pAppData->nFramecount) {
        printf("total frame count %d\n", pAppData->nFramecount);
        pBuffer->nFlags |= OMX_BUFFERFLAG_EOS;
    }


    if (pAppData->fIn) {
        if (pAppData->bMetadataMode == OMX_TRUE)
            pBuffer->nFilledLen = OMXENC_fill_metadata(pBuffer, pAppData->pInPortDef->nBufferSize);
        else
            pBuffer->nFilledLen = OMXENC_fill_data(pBuffer, pAppData->fIn, pAppData->pInPortDef->nBufferSize);
    }

    eError = OMX_EmptyThisBuffer(pAppData->pComponent, pBuffer);
    OMXENC_CHECK_ERROR(eError, "Error at EmptyThisBuffer function 1");

    pAppData->nCurrentFrameIn++;

EXIT:
    return eError;
}


//component work thread
OMX_ERRORTYPE OMXENC_onEvent_Callback(OMX_HANDLETYPE hComponent,
                                      OMX_PTR pData,
                                      OMX_EVENTTYPE eEvent,
                                      OMX_U32 nData1,
                                      OMX_U32 nData2,
                                      OMX_PTR pEventData)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;

    printf("OnEvent: event 0x%x nData1 %d nData2 : 0x%x\n", eEvent, nData1, nData2);
    omx_message msg;
    msg.type = omx_message::EVENT;
    msg.node = pData;
    msg.u.event_data.event = eEvent;
    msg.u.event_data.data1 = nData1;
    msg.u.event_data.data2 = nData2;

    if(gNothread)
    {
        OMXENC_EventHandler(pData, eEvent, nData1, nData2);
    }
    else
    {
        Mutex::Autolock autoLock(mEventLock);
        eventQueue.push_back(msg);
        mEventReceive.signal();
    }
EXIT:
    return eError;
}

//component work thread
OMX_ERRORTYPE OMXENC_onEmptyBufferDone_Callback(OMX_HANDLETYPE hComponent,
        OMX_PTR pData,
        OMX_BUFFERHEADERTYPE* pBuffer)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    printf("onEmptyBufferDone %p  (%4d)\n", pBuffer, __LINE__);
    omx_message msg;

    msg.type = omx_message::EMPTY_BUFFER_DONE;
    msg.node = pData;
    msg.u.buffer_data.buffer = pBuffer;
    if(gNothread)
    {
        OMXENC_EmptyBufferDone(pData,pBuffer);
    }
    else
    {
        Mutex::Autolock autoLock(mMsgLock);
        msgQueue.push_back(msg);
        mMessageReceive.signal();
    }
EXIT:
    return eError;
}

//component work thread
OMX_ERRORTYPE OMXENC_onFillBufferDone_Callback(OMX_HANDLETYPE hComponent,
        OMX_PTR pData,
        OMX_BUFFERHEADERTYPE* pBuffer)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    omx_message msg;

    printf("onFillBufferDone %p  (%4d)\n", pBuffer, __LINE__);
    msg.type = omx_message::FILL_BUFFER_DONE;
    msg.node = pData;
    msg.u.buffer_data.buffer = pBuffer;
    if(gNothread)
    {
        OMXENC_FillBufferDone(pData,pBuffer);
    }
    else
    {
        Mutex::Autolock autoLock(mMsgLock);
        msgQueue.push_back(msg);
        mMessageReceive.signal();
    }

EXIT:
    return eError;
}


//event thread
void *OMXENC_on_event(void *)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    omx_message msg;

    while(mRunning) {
        Mutex::Autolock autoLock(mEventLock);
        while(eventQueue.empty()) {
            mEventReceive.wait(mEventLock);
            while(!eventQueue.empty()) {
                msg = *eventQueue.begin();
                eventQueue.erase(eventQueue.begin());
                switch (msg.type) {
                case omx_message::EVENT:
                {
                    eError = OMXENC_EventHandler(msg.node,
                                                 msg.u.event_data.event,
                                                 msg.u.event_data.data1,
                                                 msg.u.event_data.data2);
                    OMXENC_CHECK_EXIT(eError, "OMXENC_EventHandlererror ");
                }
                break;
                default:
                    break;

                }
            }
        }
    }

EXIT:
    return NULL;
}



//message thread
void *OMXENC_on_message(void *)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    omx_message msg;

    while(mRunning) {
        Mutex::Autolock autoLock(mMsgLock);
        while(msgQueue.empty()) {
            mMessageReceive.wait(mMsgLock);
            while(!msgQueue.empty()) {
                msg = *msgQueue.begin();
                msgQueue.erase(msgQueue.begin());
                switch (msg.type) {
                case omx_message::EVENT:
                {
                    eError = OMXENC_EventHandler(msg.node,
                                                 msg.u.event_data.event,
                                                 msg.u.event_data.data1,
                                                 msg.u.event_data.data2);
                    OMXENC_CHECK_EXIT(eError, "OMXENC_EventHandlererror ");
                }
                break;
                case omx_message::FILL_BUFFER_DONE:
                {
                    eError = OMXENC_FillBufferDone(msg.node,
                                                   (OMX_BUFFERHEADERTYPE *)msg.u.buffer_data.buffer);
                    OMXENC_CHECK_EXIT(eError, "OMXENC_EventHandlererror ");
                }
                break;
                case omx_message::EMPTY_BUFFER_DONE:
                {
                    eError = OMXENC_EmptyBufferDone(msg.node,
                                                    (OMX_BUFFERHEADERTYPE *)msg.u.buffer_data.buffer);
                    OMXENC_CHECK_EXIT(eError, "OMXENC_EventHandlererror ");
                }
                break;
                default:
                    break;

                }
            }
        }
    }

EXIT:
    return NULL;
}

OMX_ERRORTYPE OMXENC_SetH264Parameter(MYDATATYPE* pAppData)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_HANDLETYPE pHandle = pAppData->pHandle;

    /* Set the component's OMX_VIDEO_PARAM_AVCTYPE structure (output) */
    /* OMX_VIDEO_PORTDEFINITION values for output port */
    OMX_VIDEO_PARAM_AVCTYPE h264_video_param;
    memset(&h264_video_param, 0x0, sizeof(OMX_VIDEO_PARAM_AVCTYPE));
    InitOMXParams(&h264_video_param);
    h264_video_param.nPortIndex = OMX_DirOutput;
    h264_video_param.eLevel = OMX_VIDEO_AVCLevel41;
    if (strcmp(pAppData->eProfile, "BP"))
        h264_video_param.eProfile = OMX_VIDEO_AVCProfileBaseline;
    else if (strcmp(pAppData->eProfile, "HP"))
        h264_video_param.eProfile = OMX_VIDEO_AVCProfileHigh;
    else if (strcmp(pAppData->eProfile, "MP"))
        h264_video_param.eProfile = OMX_VIDEO_AVCProfileMain;
    h264_video_param.nPFrames = pAppData->nPFrames;
    h264_video_param.nBFrames = pAppData->nBFrames;
    h264_video_param.bDirect8x8Inference = OMX_TRUE;
    h264_video_param.bEntropyCodingCABAC = OMX_TRUE;
    eError = OMX_SetParameter(pHandle, OMX_IndexParamVideoAvc, &h264_video_param);
    OMXENC_CHECK_EXIT(eError, "Error at SetParameter(IndexParamVideoAvc)");

EXIT:
    return eError;
}


void OMXENC_configCodec(MYDATATYPE* pAppData)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_HANDLETYPE pHandle = pAppData->pHandle;

    /* Set the component's OMX_PARAM_PORTDEFINITIONTYPE structure (input) */
    /* OMX_VIDEO_PORTDEFINITION values for input port */
    InitOMXParams(pAppData->pInPortDef);
    pAppData->pInPortDef->nPortIndex = OMX_DirInput;
    eError = OMX_GetParameter(pHandle, OMX_IndexParamPortDefinition, pAppData->pInPortDef);
    OMXENC_CHECK_EXIT(eError, "Error at GetParameter(IndexParamPortDefinition I)");
    pAppData->nInputBufNum = pAppData->pInPortDef->nBufferCountActual;
    pAppData->pInPortDef->nBufferSize = pAppData->nWidth * pAppData->nHeight * 3 / 2;
    pAppData->pInPortDef->format.video.eColorFormat = pAppData->eColorFormat;
    pAppData->pInPortDef->format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
    pAppData->pInPortDef->format.video.nStride = pAppData->nWidth;
    pAppData->pInPortDef->format.video.nSliceHeight = pAppData->nHeight;
    pAppData->pInPortDef->format.video.xFramerate = pAppData->nFramerate << 16;
    pAppData->pInPortDef->format.video.nFrameWidth = pAppData->nWidth;
    pAppData->pInPortDef->format.video.nFrameHeight = pAppData->nHeight;

    eError = OMX_SetParameter(pHandle, OMX_IndexParamPortDefinition, pAppData->pInPortDef);
    OMXENC_CHECK_EXIT(eError, "Error at SetParameter(IndexParamPortDefinition I)");
    /* To get nBufferSize */

    /* Set the component's OMX_PARAM_PORTDEFINITIONTYPE structure (output) */
    InitOMXParams(pAppData->pOutPortDef);
    pAppData->pOutPortDef->nPortIndex = OMX_DirOutput;
    eError = OMX_GetParameter(pHandle, OMX_IndexParamPortDefinition, pAppData->pOutPortDef);
    OMXENC_CHECK_EXIT(eError, "Error at GetParameter(IndexParamPortDefinition O)");
    pAppData->pOutPortDef->format.video.eColorFormat = (OMX_COLOR_FORMATTYPE)OMX_VIDEO_CodingUnused;
    pAppData->pOutPortDef->format.video.eCompressionFormat = pAppData->eCompressionFormat;
    pAppData->pOutPortDef->format.video.nFrameWidth = pAppData->nWidth;
    pAppData->pOutPortDef->format.video.nFrameHeight = pAppData->nHeight;
    pAppData->pOutPortDef->format.video.nStride = pAppData->nWidth;
    pAppData->pOutPortDef->format.video.nSliceHeight = pAppData->nHeight;
    pAppData->pOutPortDef->format.video.nBitrate = pAppData->nBitrate;
    pAppData->pOutPortDef->format.video.xFramerate = pAppData->nFramerate << 16;

    eError = OMX_SetParameter(pHandle, OMX_IndexParamPortDefinition, pAppData->pOutPortDef);
    OMXENC_CHECK_EXIT(eError, "Error at SetParameter(IndexParamPortDefinition O)");

    switch (pAppData->eCompressionFormat) {
    case OMX_VIDEO_CodingAVC:
        eError = OMXENC_SetH264Parameter(pAppData);
        OMXENC_CHECK_EXIT(eError, "Error returned from SetH264Parameter()");
        break;
    default:
        printf("Invalid compression format value.\n");
        eError = OMX_ErrorUnsupportedSetting;
        goto EXIT;
    }
    OMX_VIDEO_PARAM_BITRATETYPE pParamBitrate;
    InitOMXParams(&pParamBitrate);
    pParamBitrate.nPortIndex = OMX_DirOutput;
    pParamBitrate.nTargetBitrate = pAppData->nBitrate;
    pParamBitrate.eControlRate = pAppData->eControlRate;
    eError = OMX_SetParameter(pHandle,
                              (OMX_INDEXTYPE)OMX_IndexParamVideoBitrate, &pParamBitrate);
    OMXENC_CHECK_EXIT(eError, "Error at SetParameter(OMX_IndexParamIntelBitrate");


    //metadata mode setting
    if (pAppData->bMetadataMode == OMX_TRUE) {
        OMX_INDEXTYPE index;
        eError = OMX_GetExtensionIndex(pHandle,
                                       "OMX.google.android.index.storeMetaDataInBuffers",
                                       &index);
        OMXENC_CHECK_EXIT(eError, "get OMX.Intel.index.storeMetaDataInBuffers ");

        StoreMetaDataInBuffersParams params;
        memset(&params, 0, sizeof(params));
        InitOMXParams(&params);

        params.nPortIndex = OMX_DirInput;
        params.bStoreMetaData = OMX_TRUE;
        eError = OMX_SetParameter(pHandle, index, &params);
        OMXENC_CHECK_EXIT(eError, "set storeMetaDataInBuffers error");
    }

    if (pAppData->bSyncMode == OMX_TRUE) {
        OMX_INDEXTYPE index;
        eError = OMX_GetExtensionIndex(pHandle,
                                       "OMX.Intel.index.enableSyncEncoding",
                                       &index);
        OMXENC_CHECK_EXIT(eError, "get OMX.Intel.index.enableSyncEncoding ");

        OMX_BOOL enable = OMX_TRUE;
        eError = OMX_SetParameter(pHandle, index, &enable);
        OMXENC_CHECK_EXIT(eError, "Set sync encoding error");
    }

EXIT:
    return ;
}


OMX_ERRORTYPE OMXENC_Init(MYDATATYPE* pAppData)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_HANDLETYPE pHandle;
    int32_t err;
    OMX_CALLBACKTYPE sCb = {OMXENC_onEvent_Callback,
                            OMXENC_onEmptyBufferDone_Callback,
                            OMXENC_onFillBufferDone_Callback
                           };

    mRunning = OMX_TRUE;
    if(!gNothread) {
        err = pthread_create(&mThread, NULL, OMXENC_on_message, NULL);
        err = pthread_create(&mThread, NULL, OMXENC_on_event, NULL);
        if(err != 0)
            printf("can't create thread: %s\n", strerror(err));
    }

    if (pAppData->eCompressionFormat == OMX_VIDEO_CodingAVC)
        StrVideoEncoder = StrVideoEncoderAVC;

    pAppData->pInPortDef =
        (OMX_PARAM_PORTDEFINITIONTYPE *)malloc(sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
    pAppData->pOutPortDef =
        (OMX_PARAM_PORTDEFINITIONTYPE*)malloc(sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
    if ( !pAppData->pInPortDef && !pAppData->pOutPortDef) {
        printf("malloc error port definition %d\n", __LINE__);
        goto EXIT;
    }

    /* Initialize OMX Core */
    eError = OMX_Init();
    OMXENC_CHECK_EXIT(eError, "OMX_Init error");

    pAppData->sCallback.EventHandler  = OMXENC_onEvent_Callback;
    pAppData->sCallback.EmptyBufferDone  = OMXENC_onEmptyBufferDone_Callback;
    pAppData->sCallback.FillBufferDone  = OMXENC_onFillBufferDone_Callback;

    /* Get VideoEncoder Component Handle */
    eError = OMX_GetHandle(&pHandle, StrVideoEncoder, pAppData, &pAppData->sCallback);
    OMXENC_CHECK_EXIT(eError, "OMX_GetHanle error ");
    if (pHandle == NULL) {
        printf("GetHandle return Null Pointer\n");
        return OMX_ErrorUndefined;
    }

    pAppData->pHandle = pHandle;

    OMX_PARAM_COMPONENTROLETYPE cRole;
    InitOMXParams(&cRole);
    if (pAppData->eCompressionFormat == OMX_VIDEO_CodingAVC)
        strncpy((char*)cRole.cRole, "video_encoder.avc\0", 18);
    else if (pAppData->eCompressionFormat == OMX_VIDEO_CodingVP8)
        strncpy((char*)cRole.cRole, "video_encoder.vp8\0", 18);
    eError = OMX_SetParameter(pHandle, OMX_IndexParamStandardComponentRole, &cRole);
    OMXENC_CHECK_EXIT(eError, "set component role error");

    OMXENC_configCodec(pAppData);

    pAppData->eCurrentState = OMX_StateLoaded;

EXIT:
    return eError;
}


OMX_ERRORTYPE OMXENC_Run(MYDATATYPE* pAppData)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_HANDLETYPE pHandle = pAppData->pHandle;

    pAppData->pComponent = (OMX_COMPONENTTYPE*)pHandle;

    /*Initialize Frame Counter */
    pAppData->nCurrentFrameIn = 0;
    pAppData->nCurrentFrameOut = 0;
    pAppData->nByteRate = 0;
    pAppData->bStart = OMX_FALSE;
    pAppData->nWriteLen = 0;
    pAppData->nPortReEnable = OMX_FALSE;

    /* Send FillThisBuffer to OMX Video Encoder */
    for (uint32_t nCounter = 0; nCounter < pAppData->pOutPortDef->nBufferCountActual; nCounter++) {
        printf("FillThisBuffer output buffer %p \n", pAppData->pOutBuff[nCounter]);
        eError = OMX_FillThisBuffer(pAppData->pComponent, pAppData->pOutBuff[nCounter]);
        OMXENC_CHECK_EXIT(eError, "Error in FillThisBuffer");
    }


    /* Send EmptyThisBuffer to OMX Video Encoder */
    for (uint32_t nCounter = 0; nCounter < pAppData->pInPortDef->nBufferCountActual; nCounter++) {
        pAppData->nTc1 = GetTickCount();
        if (pAppData->bMetadataMode == OMX_TRUE)
            pAppData->pInBuff[nCounter]->nFilledLen =
                OMXENC_fill_metadata(pAppData->pInBuff[nCounter], pAppData->pInPortDef->nBufferSize);
        else
            pAppData->pInBuff[nCounter]->nFilledLen =
                OMXENC_fill_data(pAppData->pInBuff[nCounter],
                                 pAppData->fIn,
                                 pAppData->pInPortDef->nBufferSize);
        pAppData->nUpload += (GetTickCount() - pAppData->nTc1);

        if (pAppData->nFramecount < 5) {
            if (pAppData->nCurrentFrameIn == pAppData->nFramecount - 1) {
                printf("total frame count %d\n", pAppData->nFramecount);
                pAppData->pInBuff[nCounter]->nFlags |= OMX_BUFFERFLAG_EOS;
            }
        }

        printf("EmptyThisBuffer input buffer %p \n", pAppData->pInBuff[nCounter]);
        eError = OMX_EmptyThisBuffer(pAppData->pHandle, pAppData->pInBuff[nCounter]);
        OMXENC_CHECK_ERROR(eError, "Error at EmptyThisBuffer function");
        printf("EmptyThisBuffer done , status %d\n", pAppData->eCurrentState);

        if(gNothread && nCounter == 0)     /*Trick: wait mediasdk init its encoder*/
        {
            {
                Mutex::Autolock autoLock(mLock);
                while (pAppData->nPortReEnable != OMX_TRUE)
                    mAsyncCompletion.wait(mLock);
            }
            //tried to disable port..
            pAppData->nPortReEnable = OMX_FALSE;
            printf("Disable output port..... \n");
            {
                eError= OMX_SendCommand(pAppData->pHandle, OMX_CommandPortDisable, OMX_DirOutput,NULL);

                for (uint32_t nCounter = 0; nCounter < pAppData->pOutPortDef->nBufferCountActual; nCounter++) {
                    printf("free output buffer %p \n", pAppData->pOutBuff[nCounter]);
                    eError = OMX_FreeBuffer(pAppData->pComponent,1, pAppData->pOutBuff[nCounter]);
                    OMXENC_CHECK_EXIT(eError, "Error in freeBuffer");
                }
            }

            {
                Mutex::Autolock autoLock(mLock);
                while (pAppData->nPortReEnable != OMX_TRUE)
                    mAsyncCompletion.wait(mLock);
            }
            printf("Enable output port..... \n");
            {
                eError= OMX_SendCommand(pAppData->pHandle,  OMX_CommandPortEnable, OMX_DirOutput,NULL);
                OMXENC_allocateOnPort(pAppData, pAppData->pOutPortDef,pAppData->pOutBuff);

                for (uint32_t nCounter = 0; nCounter < pAppData->pOutPortDef->nBufferCountActual; nCounter++) {
                    printf("FillThisBuffer output buffer %p \n", pAppData->pOutBuff[nCounter]);
                    eError = OMX_FillThisBuffer(pAppData->pComponent, pAppData->pOutBuff[nCounter]);
                    OMXENC_CHECK_EXIT(eError, "Error in FillThisBuffer");
                }
            }
        }

        pAppData->nCurrentFrameIn++;
    }

    {
        Mutex::Autolock autoLock(mLock);
        while(pAppData->nFlags != OMX_BUFFERFLAG_EOS)
            mAsyncCompletion.wait(mLock);
    }
    printf("Receive EOS notify \n");
    eError = OMX_SendCommand(pAppData->pHandle, OMX_CommandFlush, OMX_ALL, NULL);
    OMXENC_CHECK_ERROR(eError, "Error at OMX_CommandFlush ");
    {
        Mutex::Autolock autoLock(mLock);
        while (pAppData->eCurrentState != OMX_StateIdle)
            mAsyncCompletion.wait(mLock);
    }

EXIT:
    return eError;
}


OMX_ERRORTYPE OMXENC_SetConfig(MYDATATYPE* pAppData)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_HANDLETYPE pHandle = pAppData->pHandle;

    if (pAppData->eCompressionFormat == OMX_VIDEO_CodingAVC) {
        OMX_VIDEO_CONFIG_AVCINTRAPERIOD intraperiodConfig;
        InitOMXParams(&intraperiodConfig);
        intraperiodConfig.nPortIndex = OMX_DirOutput;
        intraperiodConfig.nIDRPeriod = pAppData->nIDRInterval;
        intraperiodConfig.nPFrames = pAppData->nPFrames;
        eError = OMX_SetConfig(pAppData->pHandle, OMX_IndexConfigVideoAVCIntraPeriod, &intraperiodConfig);
        OMXENC_CHECK_ERROR((OMX_ERRORTYPE)eError, "Error at setConfig force IDR function");
    }
EXIT:
    return eError;
}



OMX_ERRORTYPE OMXENC_DeInit(MYDATATYPE* pAppData)
{
    OMXENC_PRINT("%s begin \n",  __FUNCTION__);
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    int eErr = 0;
    OMX_U32 nCounter;
    OMX_HANDLETYPE pHandle = pAppData->pHandle;

    for (int i = 0; i <  pAppData->nInputBufNum; i++)
        if (pAppData->handle_t[i] != NULL) {
            //freeGraphicBuf(i);
        }

    if (pAppData->memMode != MEM_MODE_GFXHANDLE) {
        for (int i = 0; i < pAppData->nInputBufNum; i++)
            if (pAppData->pUsrAddr[i] != NULL)
                free(pAppData->pUsrAddr[i]);
    }

    /* Free Component Handle */
    if (pHandle)
        eError = OMX_FreeHandle(pHandle);
    OMXENC_CHECK_EXIT(eError, "Error in OMX_FreeHandle   ===");

    /* De-Initialize OMX Core */
    eError = OMX_Deinit();
    OMXENC_CHECK_EXIT(eError, "Error in OMX_Deinit  ===");

    mRunning = OMX_FALSE;

    /* shutdown */
    if (pAppData->fIn)
        fclose(pAppData->fIn);
    fclose(pAppData->fOut);

    if (pAppData->pfVP8File != NULL) {
        fclose(pAppData->pfVP8File);
        pAppData->pfVP8File = NULL;
    }

    pAppData->fIn = NULL;
    pAppData->fOut = NULL;
    free(pAppData->pInPortDef);
    free(pAppData->pOutPortDef);

    free(pAppData);
    pAppData = NULL;

EXIT:
    OMXENC_PRINT("%s end\n",  __FUNCTION__);
    return eError;
}



OMX_ERRORTYPE OMXENC_allocateOnPort(MYDATATYPE* pAppData,
                                    OMX_PARAM_PORTDEFINITIONTYPE* port,
                                    OMX_BUFFERHEADERTYPE** buf)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;

    InitOMXParams(port);
    eError = OMX_GetParameter(pAppData->pHandle, OMX_IndexParamPortDefinition, port);
    OMXENC_CHECK_EXIT(eError, "Error at GetParameter(IndexParamPortDefinition I)");
    printf("%s Width %d, Height %d, nStride %d, nBufferSize %d, eColorFormat %d xFramerate %d buffer num %d\n",
           port->nPortIndex == OMX_DirInput ? "Input" : "Output",
           port->format.video.nFrameWidth,
           port->format.video.nFrameHeight,
           port->format.video.nStride,
           port->nBufferSize,
           port->format.video.eColorFormat,
           port->format.video.xFramerate >> 16,
           port->nBufferCountActual);

    for (int32_t nCounter = 0; nCounter < port->nBufferCountActual; nCounter++) {
        eError = OMX_AllocateBuffer(pAppData->pHandle,
                                    &buf[nCounter],
                                    port->nPortIndex,
                                    pAppData,
                                    port->nBufferSize);
        printf("%s, buf %p \n",
               port->nPortIndex == OMX_DirInput ? "Input" : "Output", buf[nCounter]);

        OMXENC_CHECK_EXIT(eError, "Error OMX_AllocateBuffer");
    }

EXIT:
    return eError;
}

OMX_ERRORTYPE OMXENC_AllocBuffer(MYDATATYPE* pAppData)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    eError = OMXENC_allocateOnPort(pAppData, pAppData->pOutPortDef, pAppData->pOutBuff);
    eError = OMXENC_allocateOnPort(pAppData, pAppData->pInPortDef, pAppData->pInBuff);
    return eError;
}


OMX_ERRORTYPE OMXENC_FreeBuffer(MYDATATYPE* pAppData)
{
    OMXENC_PRINT("%s begin\n", __FUNCTION__);
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_HANDLETYPE pHandle = pAppData->pHandle;
    OMX_U32 nCounter;

    for (nCounter = 0; nCounter < pAppData->pOutPortDef->nBufferCountActual; nCounter++)
        eError = OMX_FreeBuffer(pHandle,
                                pAppData->pOutPortDef->nPortIndex, pAppData->pOutBuff[nCounter]);
    OMXENC_CHECK_EXIT(eError, "Error at OMX_FreeBuffer function");



    for (nCounter = 0; nCounter < pAppData->pInPortDef->nBufferCountActual; nCounter++)
        eError = OMX_FreeBuffer(pHandle,
                                pAppData->pInPortDef->nPortIndex, pAppData->pInBuff[nCounter]);
    OMXENC_CHECK_EXIT(eError, "Error at OMX_FreeBuffer function");

EXIT:
    OMXENC_PRINT("%s end\n",  __FUNCTION__);
    return eError;
}

OMX_ERRORTYPE OMXENC_SetState(MYDATATYPE* pAppData, OMX_STATETYPE state)
{
    OMXENC_PRINT("%s begin\n", __FUNCTION__);
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_HANDLETYPE pHandle = pAppData->pHandle;

    eError = OMX_SendCommand(pHandle, OMX_CommandStateSet, state, NULL);
    OMXENC_CHECK_EXIT(eError, "Error SendCommand OMX_CommandStateSet");
    if( state==OMX_StateIdle && pAppData->eCurrentState == OMX_StateLoaded )
    {
        eError = OMXENC_allocateOnPort(pAppData, pAppData->pOutPortDef, pAppData->pOutBuff);
        eError = OMXENC_allocateOnPort(pAppData, pAppData->pInPortDef, pAppData->pInBuff);
    }
    if( state==OMX_StateLoaded && pAppData->eCurrentState == OMX_StateIdle)
    {
        eError = OMXENC_FreeBuffer(pAppData);
        OMXENC_CHECK_EXIT(eError, "Error OMXENC_FreeBuffer");
    }

    OMXENC_PRINT("wait omx component change its state to [%d], current state [%d]\n", state, pAppData->eCurrentState);

    {
        Mutex::Autolock autoLock(mLock);
        while (pAppData->eCurrentState != state)
            mAsyncCompletion.wait(mLock);
    }

EXIT:
    OMXENC_PRINT("%s end \n", __FUNCTION__);
    return eError;
}

void PrintHelpInfo(void)
{
    printf("./omx_encode <options>\n");
    printf("   -t <H264>\n");
    printf("   -w <width> -h <height>\n");
    printf("   -framecount [num]\n");
    printf("   -n [num]\n");
    printf("   -o <coded file>\n");
    printf("   -f [num]\n");
    printf("   -npframe [num] number of P frames between I frames\n");
    printf("   -nbframe [num] number of B frames between I frames\n");
    printf("   -idrinterval [num]  Specifics the number of I frames between two IDR frames (for H264 only)\n");
    printf("   -bitrate [num]\n");
    printf("   -rcMode <NONE|CBR|VBR|VCM|CQP|VBR_CONTRAINED>\n");
    printf("   -syncmode: sequentially upload source, encoding, save result, no multi-thread\n");
    printf("   -metadata: metadata mode enable\n");
    printf("   -surface <malloc|gralloc> \n");
    printf("   -srcyuv <filename> load YUV from a file\n");
    printf("   -fourcc <NV12|IYUV|YV12> source YUV fourcc\n");
    printf("   -profile <BP|MP|HP>\n");
    printf("   -level <level>\n");
    printf("   -airenable Enable AIR\n");
    printf("   -airauto AIR auto\n");
    printf("   -airmbs [num] minimum number of macroblocks to refresh in a frame when adaptive intra-refresh (AIR) is enabled\n");
    printf("   -air_threshold [num] AIR threshold\n");
    printf("   -pbitrate show dynamic bitrate result\n");
    printf("   -num_cir_mbs [num] number of macroblocks to be coded as intra when cyclic intra-refresh (CIR) is enabled\n");
    printf("example:\n\t omxenc -w 1920 -h 1080 -bitrate 10000000 -metadata -surface gralloc -intra_period 30 -idrinterval 2\n");
}

static int print_input(MYDATATYPE* pAppData)
{
    printf("\n");
    printf("INPUT:    Try to encode %s...\n", pAppData->szCompressionFormat);
    printf("INPUT:    Resolution   : %dx%d, %d frames\n",
           pAppData->nWidth, pAppData->nHeight, pAppData->nFramecount);
    printf("INPUT:    Source YUV   : %s", pAppData->szInFile ? "FILE" : "AUTO generated");
    printf("\n");

    printf("INPUT:    RateControl  : %s\n", pAppData->szControlRate);
    printf("INPUT:    FrameRate    : %d\n", pAppData->nFramerate);
    printf("INPUT:    Bitrate      : %d(initial)\n", pAppData->nBitrate);
    //printf("INPUT:    Slieces      : %d\n", frame_slice_count);
    printf("INPUT:    IntraPeriod  : %d\n", pAppData->nPFrames + 1);
    printf("INPUT:    IDRPeriod    : %d\n", pAppData->nIDRInterval * (pAppData->nPFrames + 1));
    printf("INPUT:    IpPeriod     : %d\n", pAppData->nBFrames + 1);
    printf("INPUT:    Initial QP   : %d\n", pAppData->nInitQP);
    printf("INPUT:    Min QP       : %d\n", pAppData->nMinQP);
    printf("INPUT:    Layer Number : %d\n", pAppData->number_of_layer);
    printf("\n"); /* return back to startpoint */

    return 0;
}

OMX_ERRORTYPE Create_AppData(MYDATATYPE** pAppDataTmp, int argc, char** argv)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    MYDATATYPE* pAppData;

    pAppData = (MYDATATYPE*)malloc(sizeof(MYDATATYPE));
    if (!pAppData) {
        printf("malloc(pAppData) error. %d\n", __LINE__);
        exit(0);
    }
    memset(pAppData, 0, sizeof(MYDATATYPE));

    *pAppDataTmp = pAppData;
    //pAppData->eNaluFormat = OMX_NaluFormatStartCodes;
    pAppData->eNaluFormat = (OMX_NALUFORMATSTYPE)OMX_NaluFormatStartCodesSeparateFirstHeader;
    pAppData->eCurrentState = OMX_StateInvalid;
    pAppData->nQPIoF = 0;
    pAppData->nFlags = 0;
    pAppData->nISliceNum = 2;
    pAppData->nPSliceNum = 2;
    pAppData->nIDRInterval = 0;
    pAppData->nPFrames = 20;
    pAppData->nBFrames = 0;
    pAppData->nSavedFrameNum = 0;
    pAppData->nFramecount = 50;
    pAppData->memMode == MEM_MODE_MALLOC;
    memcpy(pAppData->szInFile, "auto", 4);
    memcpy(pAppData->szOutFile, "/data/out.264", 13);
    pAppData->nWidth = 320;
    pAppData->nHeight = 240;
    pAppData->nSliceSize = 0;
    pAppData->nGOBHeaderInterval = 30;
    pAppData->eColorFormat = OMX_COLOR_FormatYUV420Planar;
    pAppData->nFramerate = 30;
    pAppData->nBitrate = 15000000;
    pAppData->eCompressionFormat = OMX_VIDEO_CodingAVC;
    strncpy(pAppData->szCompressionFormat, "h264", 4);
    pAppData->eControlRate = OMX_Video_ControlRateVariable;
    strncpy(pAppData->szControlRate, "vbr", 3);
    pAppData->eLevel = OMX_VIDEO_AVCLevel31;
    strncpy(pAppData->eProfile, "BP", 2);
    pAppData->nMinQP = 0;
    pAppData->nInitQP = 0;
    pAppData->nMaxQP = 51;
    pAppData->nMaxEncodeBitrate = 15000000;
    pAppData->nTargetPercentage = 66;
    pAppData->nWindowSize = 1000;
    pAppData->bFrameSkip = OMX_FALSE;
    pAppData->bShowPic = OMX_FALSE;
    pAppData->nDisableBitsStuffing =  0;
    pAppData->bMetadataMode = OMX_FALSE;
    pAppData->bSyncMode = OMX_FALSE;
    pAppData->bShowBitrateRealTime = OMX_FALSE;
    pAppData->number_of_rewind = 0;
    //config_fn = "/data/config-omx-test";
    //AIR
    pAppData->bAirAuto = OMX_FALSE;
    pAppData->bAirEnable = OMX_FALSE;
    pAppData->nAirMBs = 0;
    pAppData->nAirThreshold = 0;

    //Intra refresh
    pAppData->eRefreshMode = OMX_VIDEO_IntraRefreshAdaptive;
    pAppData->nIrRef = 0;
    pAppData->nCirMBs = 0;

    //
    pAppData->bSNAlter = OMX_FALSE;
    pAppData->bRCAlter = OMX_FALSE;
    pAppData->bDeblockFilter = OMX_FALSE;
    //
    pAppData->only_render_k_frame_one_time = 0;
    pAppData->max_frame_size_ratio = 0;


    const struct option long_opts[] = {
        {"help", no_argument, NULL, 0 },
        {"bitrate", required_argument, NULL, 1 },
        {"minqp", required_argument, NULL, 2 },
        {"initialqp", required_argument, NULL, 3 },
        {"intra_period", required_argument, NULL, 4 },
        {"idr_period", required_argument, NULL, 5 },
        {"ip_period", required_argument, NULL, 6 },
        {"rcMode", required_argument, NULL, 7 },
        {"srcyuv", required_argument, NULL, 9 },
        {"recyuv", required_argument, NULL, 10 },
        {"fourcc", required_argument, NULL, 11 },
        {"syncmode", no_argument, NULL, 12 },
        {"enablePSNR", no_argument, NULL, 13 },
        {"surface", required_argument, NULL, 14 },
        {"priv", required_argument, NULL, 15 },
        {"framecount", required_argument, NULL, 16 },
        {"entropy", required_argument, NULL, 17 },
        {"profile", required_argument, NULL, 18 },
        {"sliceqp", required_argument, NULL, 19 },
        {"level", required_argument, NULL, 20 },
        {"stridealign", required_argument, NULL, 21 },
        {"heightalign", required_argument, NULL, 22 },
        {"surface", required_argument, NULL, 23 },
        {"slices", required_argument, NULL, 24 },
//        {"configfile", required_argument, NULL, 25 },
        {"quality", required_argument, NULL, 26 },
        {"autokf", required_argument, NULL, 27 },
        {"kf_dist_min", required_argument, NULL, 28 },
        {"kf_dist_max", required_argument, NULL, 29 },
        {"error_resilient", required_argument, NULL, 30 },
        {"maxqp", required_argument, NULL, 31},
        {"bit_stuffing", no_argument, NULL, 32},
//        {"plog", no_argument, NULL, 33},
        {"metadata", no_argument, NULL, 34},
        {"qpi", no_argument, NULL, 35},
        {"vuiflag", no_argument, NULL, 36},
        {"forceIDR", no_argument, NULL, 37},
        {"refreshIntraPeriod", no_argument, NULL, 38},
        {"windowsize", no_argument, NULL, 39},
        {"maxencodebitrate",  required_argument, NULL, 40},
        {"targetpercentage",   required_argument, NULL, 41},
        {"islicenum", required_argument, NULL, 42},
        {"pslicenum", required_argument, NULL, 43},
        {"idrinterval", required_argument, NULL, 44},
        {"npframe", required_argument, NULL, 45},
        {"nbframe", required_argument, NULL, 46},
        {"rc_alter", no_argument, NULL, 47},
        {"bit_stuffing_dis", no_argument, NULL, 48},
        {"frame_skip_dis", no_argument, NULL, 49},
        {"num_cir_mbs", required_argument, NULL, 50},
        {"maxslicesize", required_argument, NULL, 51},
        {"intrarefresh", required_argument, NULL, 52},
        {"airmbs", required_argument, NULL, 53},
        {"airref", required_argument, NULL, 54},
        {"airenable", no_argument, NULL, 56},
        {"airauto", no_argument, NULL, 57},
        {"air_threshold", required_argument, NULL, 58},
        {"pbitrate", no_argument, NULL, 59},
        {"show_pic", no_argument, NULL, 60},
        {"max_frame_size_ratio", required_argument, NULL, 61},

        {"hh", no_argument, NULL, 69},
        {NULL, no_argument, NULL, 0 }
    };

    int long_index, i, tmp, intra_idr_period_tmp = -1;
    char c;
    int argNum;
    while ((c = getopt_long_only(argc, argv, "t:w:h:n:r:f:o:?", long_opts, &long_index)) != EOF) {
        switch (c) {
        case 69:
        case 0:
            PrintHelpInfo();
            exit(0);
        case 33:
            break;
        case 59:
            pAppData->bShowBitrateRealTime = OMX_TRUE;
            break;
        case 60: //depend on the common load_surface.h has the blend picture fucntion
            pAppData->bShowPic = OMX_TRUE;
            break;
        case 9:
            strncpy(pAppData->szInFile, optarg, strlen(optarg));
            pAppData->fIn = fopen(pAppData->szInFile, "r");
            if (!pAppData->fIn) {
                printf("Failed to open input file <%s>", pAppData->szInFile);
                return OMX_ErrorBadParameter;
            }
            break;
        case 'w':
            pAppData->nWidth = atoi(optarg);
            break;
        case 'h':
            pAppData->nHeight = atoi(optarg);
            break;
        case 'f':
            pAppData->nFramerate = atoi(optarg);
            break;
        case  1:
            pAppData->nBitrate = strtol(optarg, NULL, 0);
            break;
        case 14:
            if (!strcmp(optarg, "gralloc"))
                pAppData->memMode = MEM_MODE_GFXHANDLE;
            else if (!strcmp(optarg, "malloc"))
                pAppData->memMode = MEM_MODE_MALLOC;
            else
                pAppData->memMode = MEM_MODE_MALLOC;
            break;
        case 't':
            //Select encoding type
            memset(pAppData->szCompressionFormat, 0x00, 128 * sizeof(char));
            strncpy(pAppData->szCompressionFormat, optarg, strlen(optarg));
            if (!strcmp(optarg, "H264")) {
                pAppData->eCompressionFormat = OMX_VIDEO_CodingAVC;
            } else {
                PrintHelpInfo();
                exit(0);
            }
            break;
        case 20:
            pAppData->eLevel = atoi(optarg);
            break;
        case 'o':
            memset(pAppData->szOutFile, 0 , sizeof(pAppData->szOutFile));
            strncpy(pAppData->szOutFile, optarg, strlen(optarg));
            break;
        case 18:
            strncpy(pAppData->eProfile, optarg, strlen(optarg));
            break;
        case 34:
            pAppData->bMetadataMode = OMX_TRUE;
            break;
        case 12:
            pAppData->bSyncMode = OMX_TRUE;
            gNothread = 1;
            break;
        case 7:
            //Select encoding type
            memset(pAppData->szControlRate, 0x00, sizeof(pAppData->szControlRate));
            strncpy(pAppData->szControlRate, optarg, strlen(optarg));
            if (!strcmp(optarg, "CBR")) {
                pAppData->eControlRate = OMX_Video_ControlRateConstant;
            } else if (!strcmp(optarg, "VBR")) {
                pAppData->eControlRate = OMX_Video_ControlRateVariable;
            } else if (!strcmp(optarg, "VCM")) {
                pAppData->eControlRate =
                    (OMX_VIDEO_CONTROLRATETYPE)OMX_Video_Intel_ControlRateVideoConferencingMode;
            } else if (!strcmp(optarg, "disable")) {
                pAppData->eControlRate = OMX_Video_ControlRateDisable;
            } else {
                pAppData->eControlRate = OMX_Video_ControlRateVariable;
            }
            break;
        case 4:
            pAppData->nPFrames = atoi(optarg) - 1;
            break;
        case 5:
            pAppData->nIDRInterval = atoi(optarg);
            break;
        case 45:
            pAppData->nPFrames = atoi(optarg);
            break;
        case 'n':
        case 16:
            pAppData->nFramecount = atoi(optarg);
            break;
        case 35:
            pAppData->nQpI = atoi(optarg);
            break;
        case 25:
            break;
        case 36:
            pAppData->bVUIEnable = OMX_TRUE;
            break;
        case 2:
            pAppData->nMinQP = atoi(optarg);
            break;
        case 3:
            pAppData->nInitQP = atoi(optarg);
            break;
        case 31:
            pAppData->nMaxQP = atoi(optarg);
            break;
        case 39:
            pAppData->nWindowSize = atoi(optarg);
            break;
        case 40:
            pAppData->nMaxEncodeBitrate = atoi(optarg);
            break;
        case 41:
            pAppData->nTargetPercentage = atoi(optarg);
            break;
        case 42:
            pAppData->nISliceNum = atoi(optarg);
            break;
        case 43:
            pAppData->nPSliceNum = atoi(optarg);
            break;
        case 44:
            pAppData->nIDRInterval = atoi(optarg);
            break;
        case 46:
            pAppData->nBFrames = atoi(optarg);
            break;
        case 47:
            pAppData->bRCAlter = OMX_TRUE;
            break;
        case 48:
            pAppData->nDisableBitsStuffing = 1;
            break;
        case 49:
            pAppData->bFrameSkip = OMX_TRUE;
            break;
        case 50:
            pAppData->nCirMBs = atoi(optarg);
            break;
        case 51:
            pAppData->nSliceSize = atoi(optarg);
            if (pAppData->nSliceSize > pAppData->nWidth * pAppData->nHeight * 3 / 2) {
                printf("error slice size setting\n");
                exit(0);
            }
            break;
        case 52:
            pAppData->eRefreshMode = (OMX_VIDEO_INTRAREFRESHTYPE)atoi(optarg);
            break;
        case 53:
            pAppData->nAirMBs = atoi(optarg);
            break;
        case 54:
            pAppData->nIrRef = atoi(optarg);
            break;
        case 56:
            pAppData->bAirEnable = OMX_TRUE;
            break;
        case 57:
            pAppData->bAirAuto = OMX_TRUE;
            break;
        case 58:
            pAppData->nAirThreshold = atoi(optarg);
            break;
        case 61:
            pAppData->max_frame_size_ratio = atoi(optarg);
            break;
        case 62:
            pAppData->number_of_layer = atoi(optarg);
            break;
        case 63:
            pAppData->nBitrateForLayer0 = strtol(optarg, NULL, 0);
            break;
        case 64:
            pAppData->nBitrateForLayer1 = strtol(optarg, NULL, 0);
            break;
        case 65:
            pAppData->nBitrateForLayer2 = strtol(optarg, NULL, 0);
            break;
        case 66:
            pAppData->nFramerateForLayer0 = strtol(optarg, NULL, 0);
            break;
        case 67:
            pAppData->nFramerateForLayer1 = strtol(optarg, NULL, 0);
            break;
        case 68:
            pAppData->nFramerateForLayer2 = strtol(optarg, NULL, 0);
            break;


        }
    }

    pAppData->fOut = fopen(pAppData->szOutFile, "w");
    if (!pAppData->fOut) {
        printf("Failed to open output file <%s>\n", pAppData->szOutFile);
        return OMX_ErrorBadParameter;
    }

    print_input(pAppData);
EXIT:
    return eError;
}



int main(int argc, char** argv)
{
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    MYDATATYPE *pAppData;
    printf("===Create App data......\n");
    eError = Create_AppData( &pAppData, argc, argv);
    OMXENC_CHECK_EXIT(eError, "Create_AppData");
    printf("===Succeed!\n");

    PerfBegin(pAppData);

    printf("===Init Omx Component......\n");
    eError = OMXENC_Init(pAppData);
    OMXENC_CHECK_ERROR(eError, "Error OMXENC_Init");
    printf("===Succeed!OMX Component state [Loaded]\n");

    printf("===Set Omx Component from Loaded to Idle......\n");
    eError = OMXENC_SetState(pAppData,OMX_StateIdle);
    OMXENC_CHECK_EXIT(eError, "Error OMXENC_SetState From Loaded to Idle");
    printf("===Succeed!OMX Component state [IDLE]\n");

    printf("===Set Omx Component from Idle to Executing......\n");
    eError = OMXENC_SetState(pAppData,OMX_StateExecuting);
    OMXENC_CHECK_EXIT(eError, "Error OMXENC_SetState From Idle to Executing");
    printf("===Succeed!OMX Component state [Executing]\n");

    printf("===Configure Omx Component......\n");
    eError = OMXENC_SetConfig(pAppData);
    OMXENC_CHECK_EXIT(eError, "Error OMXENC_SetConfig\n");
    printf("===Succeed!\n");

    printf("===Start Encoding......\n");
    eError = OMXENC_Run(pAppData);
    OMXENC_CHECK_ERROR(eError, "Error at  OMXENC_Run");
    printf("===Finished!\n");

    printf("===Set Omx Component from Executing to Idle ......\n");
    eError = OMXENC_SetState(pAppData, OMX_StateIdle);
    OMXENC_CHECK_EXIT(eError, "Error OMXENC_SetState From Executing to Idle");
    printf("===Succeed!OMX Component state [IDLE]\n");

    printf("===Set Omx Component from Idle to Loaded......\n");
    eError = OMXENC_SetState(pAppData, OMX_StateLoaded);
    OMXENC_CHECK_EXIT(eError, "Error OMXENC_SetState From Idle to Loaded");
    printf("===Succeed!OMX Component state [Loaded]\n");

    printf("===DeInit....\n");
    eError = OMXENC_DeInit(pAppData);
    OMXENC_CHECK_EXIT(eError, "Error OMXENC_DeInit");
    printf("===Succeed!\n");

    PerfEnd(pAppData);
    DumpPerf(pAppData);

EXIT:
    exit(0);
    return eError;
}

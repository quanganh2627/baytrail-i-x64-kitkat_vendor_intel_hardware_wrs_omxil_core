#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sched.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <time.h>
#include <OMX_Component.h>
#include <OMX_Core.h>
#include <OMX_IndexExt.h>
#include <OMX_VideoExt.h>
#include <OMX_IntelVideoExt.h>
#include <OMX_IntelIndexExt.h>
#include <OMX_IntelErrorTypes.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/time.h>

#include <cutils/log.h>
#include <utils/Log.h>
#include <utils/List.h>
#include <utils/String16.h>
#include <utils/Errors.h>
#include <utils/Condition.h>
#include <utils/Mutex.h>
#include <string.h>
#include <HardwareAPI.h>

#include <va/va.h>
#include "loadsurface_yuv.h"
using namespace android;

#if 1
#define OMXENC_PRINT(STR, ARG...) \
	do{\
		printf(STR, ##ARG);\
	        __android_log_print(ANDROID_LOG_ERROR, "OMXENC", STR, ##ARG);\
		}while(0);
#else
#define OMXENC_PRINT LOGE
#define LOG_TAG "OMXENCTEST_jgl"
#endif

#define OMXENC_CHECK_ERROR(_e_, _s_)                \
  if (_e_ != OMX_ErrorNone){                        \
    printf("\n%s::%d ERROR %x : %s \n", __FUNCTION__, __LINE__,  _e_, _s_); \
    OMXENC_HandleError(pAppData, _e_);              \
    goto EXIT;                                      \
  }                                                 \
 
#define OMXENC_CHECK_EXIT(_e_, _s_)                 \
  if (_e_ != OMX_ErrorNone){                        \
    OMXENC_PRINT("\n%s ::%d ERROR %x : %s \n", __FUNCTION__, __LINE__, _e_, _s_); \
    goto EXIT;                                      \
  }

// libmix defines
typedef enum
{
    MEM_MODE_MALLOC = 1,
    MEM_MODE_CI = 2,
    MEM_MODE_V4L2 = 4,
    MEM_MODE_SURFACE = 8,
    MEM_MODE_USRPTR = 16,
    MEM_MODE_GFXHANDLE = 32,
    MEM_MODE_KBUFHANDLE = 64,
    MEM_MODE_ION = 128,
    MEM_MODE_NONECACHE_USRPTR = 256,
} MemoryMode;


#define NUM_OF_IN_BUFFERS 10
#define NUM_OF_OUT_BUFFERS 10
#define MAX_NUM_OF_PORTS 16
#define MAX_EVENTS 256


struct omx_message {
    enum {
        EVENT,
        EMPTY_BUFFER_DONE,
        FILL_BUFFER_DONE,

    } type;

    void* node;

    union {
        // if type == EVENT
        struct {
            OMX_EVENTTYPE event;
            OMX_U32 data1;
            OMX_U32 data2;
        } event_data;

        // if type == EMPTY/FILL_BUFFER_DONE
        struct {
            void* buffer;
        } buffer_data;
    } u;
};

typedef enum OMXENC_PORT_INDEX {
    VIDENC_INPUT_PORT = 0x0,
    VIDENC_OUTPUT_PORT
} OMXENC_PORT_INDEX;

typedef struct MYDATATYPE {
    OMX_HANDLETYPE pHandle;
    char szInFile[128];
    char szOutFile[128];
    char *szOutFileNal;
    int nWidth;
    int nHeight;
    int nFlags;
    OMX_COLOR_FORMATTYPE eColorFormat;
    OMX_U32 nBitrate;
    OMX_U32 nFramerate;
    OMX_VIDEO_CODINGTYPE eCompressionFormat;
    OMX_NALUFORMATSTYPE eNaluFormat;
    char szCompressionFormat[128];
    OMX_U32 eLevel;
    char eProfile[32];
    OMX_STATETYPE eState;
    OMX_PARAM_PORTDEFINITIONTYPE* pPortDef[MAX_NUM_OF_PORTS];
    OMX_PARAM_PORTDEFINITIONTYPE* pInPortDef;
    OMX_PARAM_PORTDEFINITIONTYPE* pOutPortDef;
    OMX_U32 nInputBufNum;
    OMX_U32 index_metadata;
    uint8_t *pUsrAddr[NUM_OF_IN_BUFFERS];
    buffer_handle_t handle_t[NUM_OF_IN_BUFFERS];
    FILE* fIn;
    FILE* fOut;

    OMX_U32 nCurrentFrameIn;
    OMX_U32 nCurrentFrameOut;
    OMX_CALLBACKTYPE sCallback;
    OMX_COMPONENTTYPE* pComponent;
    OMX_BUFFERHEADERTYPE* pInBuff[NUM_OF_IN_BUFFERS];
    OMX_BUFFERHEADERTYPE* pOutBuff[NUM_OF_OUT_BUFFERS];

    OMX_BOOL bSyncMode;
    OMX_BOOL bShowPic;
    OMX_VIDEO_CONTROLRATETYPE eControlRate;
    char szControlRate[128];
    OMX_U32 nQpI;
    OMX_BOOL bShowBitrateRealTime;
    OMX_BOOL bMetadataMode;
    OMX_BOOL bDeblockFilter;
    OMX_U32 nTargetBitRate;
    OMX_U32 nGOBHeaderInterval;
    OMX_U32 nPorts;
    void* pEventArray[MAX_EVENTS];

    OMX_STATETYPE eCurrentState;
    OMX_U32  nPortReEnable;
    OMX_U32  nQPIoF;
    unsigned int nWriteLen;
    unsigned int nBitrateSize;
    OMX_U32 nSavedFrameNum;
    //
    OMX_U32 nISliceNum;
    OMX_U32 nPSliceNum;
    OMX_U32 nSliceSize;
    //
    OMX_U32 nIDRInterval;
    int nPFrames;
    //OMX_U32 nPFrames;
    OMX_U32 nBFrames;
    OMX_VIDEO_CONFIG_NALSIZE *pNalSize;
    OMX_U32 nFramecount;

    //AIR
    OMX_BOOL bAirEnable;
    OMX_BOOL bAirAuto;
    float nAirMBs;
    OMX_U32 nAirThreshold;

    //Intre refresh
    OMX_VIDEO_INTRAREFRESHTYPE eRefreshMode;
    OMX_U32 nIrMBs;
    OMX_U32 nIrRef;
    OMX_U32 nCirMBs;

    //OMX_VIDEO_CONFIG_INTEL_BITRATETYPE
    OMX_U32 nInitQP;
    OMX_U32 nMinQP;
    OMX_U32 nMaxQP;
    OMX_U32 nMaxEncodeBitrate;
    OMX_U32 nTargetPercentage;
    OMX_U32 nWindowSize;
    OMX_BOOL bFrameSkip;
    OMX_U32 nDisableBitsStuffing;
    //
    OMX_U32 memMode;
    //
    OMX_U32 nByteRate;
    OMX_U32 nBufToRead;
    OMX_BOOL bStart;
    OMX_BOOL bRCAlter;
    OMX_BOOL bSNAlter;
    OMX_BOOL bVUIEnable;
    //statistics
    int nEncBegin, nEncEnd;
    int nUpload, nDownload, nTc1, nTc2;

    //for vp8 encoder
    FILE *pfVP8File;
    unsigned int vp8_coded_frame_size;
    unsigned int pts;
    unsigned int number_of_rewind;
    struct rc_param_t *rc_param_ptr;
    unsigned int only_render_k_frame_one_time;
    unsigned int max_frame_size_ratio;
    unsigned int number_of_layer;
    OMX_U32 nBitrateForLayer0;
    OMX_U32 nBitrateForLayer1;
    OMX_U32 nBitrateForLayer2;
    OMX_U32 nFramerateForLayer0;
    OMX_U32 nFramerateForLayer1;
    OMX_U32 nFramerateForLayer2;
    OMX_U32 nSendFrameCount;

} MYDATATYPE;

typedef struct EVENT_PRIVATE {
    OMX_EVENTTYPE eEvent;
    OMX_U32 nData1;
    OMX_U32 nData2;
    MYDATATYPE* pAppData;
    OMX_PTR pEventData;
} EVENT_PRIVATE;



template<class T>
static void InitOMXParams(T *params)
{
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

OMX_ERRORTYPE OMXENC_AllocBuffer(MYDATATYPE* pAppData);
OMX_ERRORTYPE OMXENC_FreeBuffer(MYDATATYPE* pAppData);
OMX_ERRORTYPE OMXENC_DeInit(MYDATATYPE* pAppData);

OMX_ERRORTYPE OMXENC_HandleError(MYDATATYPE* pAppData, OMX_ERRORTYPE eError);

void OMXENC_FreeResources(MYDATATYPE* pAppData);

OMX_ERRORTYPE OMXENC_allocateOnPort(MYDATATYPE* pAppData,
                                    OMX_PARAM_PORTDEFINITIONTYPE* port,
                                    OMX_BUFFERHEADERTYPE** buf);


int alloc_gralloc_buffer(int num_buffers, int ,int, buffer_handle_t *buffers);

void *lockGraphicBuf(int idx);


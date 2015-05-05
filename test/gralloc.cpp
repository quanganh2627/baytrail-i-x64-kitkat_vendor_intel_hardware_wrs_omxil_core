#include <getopt.h>

#include <ui/PixelFormat.h>
#include <GraphicBuffer.h>

#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/ISurfaceComposer.h>
#include <ui/PixelFormat.h>
#include <ui/DisplayInfo.h>
#include <system/window.h>


#include "omxenc.h"

#define BUFFER_USAGE (GraphicBuffer::USAGE_SW_READ_RARELY)|(GraphicBuffer::USAGE_SW_WRITE_RARELY)

#ifndef HAL_PIXEL_FORMAT_NV12
#define HAL_PIXEL_FORMAT_NV12 0x3231564E /*0x7FA00E00:VED*/
#endif

#ifndef HAL_PIXEL_FORMAT_NV12_Y_TILED_INTEL
#define HAL_PIXEL_FORMAT_NV12_Y_TILED_INTEL 0x100
#endif

#ifndef HAL_PIXEL_FORMAT_NV12_LINEAR_CAMERA_INTEL
#define HAL_PIXEL_FORMAT_NV12_LINEAR_CAMERA_INTEL  0x10F
#endif


static hw_module_t const *gModule;
static gralloc_module_t const *gAllocMod; /* get by force hw_module_t */
static alloc_device_t  *gAllocDev; /* get by gralloc_open */


static  GraphicBuffer *GfxBuffer[4];
int alloc_gralloc_buffer(int num_buffers, int width, int height, buffer_handle_t *buffers)
{
    int i, stride = 0;
    int pixelFormat;
    //  if(surface_type == SURFACE_TYPE_GRALLOC_LINEAR){
    //      pixelFormat = HAL_PIXEL_FORMAT_NV12_LINEAR_CAMERA_INTEL; //Linear
//	printf("Using linear gralloc buffer\n");
    //  }else{
    pixelFormat = HAL_PIXEL_FORMAT_NV12_Y_TILED_INTEL; //Y-Tiled
    printf("Using tiled gralloc buffer\n");
    //}


    for (i=0; i < num_buffers; i++) {
        ANativeWindowBuffer *WinBuffer;

        GfxBuffer[i] = new GraphicBuffer(width, height,
                                         pixelFormat, BUFFER_USAGE);
        if (GfxBuffer[i] == NULL) {
            printf("Allocate GraphicBuffer failed\n");
            exit(1);
        }
        WinBuffer = GfxBuffer[i]->getNativeBuffer();
        buffers[i] = (buffer_handle_t )WinBuffer->handle;

        stride = WinBuffer->stride;
    }
    /* gralloc buffer with it's own stride ? */
    if (width != 0 && stride != width) {
        printf("Warning: desired stride is %d, gralloc buffer stride is %d, reset stride to %d\n",
               width, stride, stride);
        width = stride;
    }

    return 0;
}

void *lockGraphicBuf(int idx) {
    void *paddr = NULL;
    GfxBuffer[idx]->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, &paddr);
    return paddr;

}

void unlockGraphicBuf(int idx) {
    GfxBuffer[idx]->unlock();
}


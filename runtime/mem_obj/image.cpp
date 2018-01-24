/*
 * Copyright (c) 2018, Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "runtime/command_queue/command_queue.h"
#include "runtime/context/context.h"
#include "runtime/helpers/surface_formats.h"
#include "runtime/device/device.h"
#include "runtime/helpers/aligned_memory.h"
#include "runtime/helpers/basic_math.h"
#include "runtime/helpers/get_info.h"
#include "runtime/helpers/hw_info.h"
#include "runtime/helpers/ptr_math.h"
#include "runtime/helpers/string.h"
#include "runtime/mem_obj/image.h"
#include "runtime/mem_obj/buffer.h"
#include "runtime/memory_manager/memory_manager.h"
#include "runtime/os_interface/debug_settings_manager.h"
#include "runtime/gmm_helper/gmm_helper.h"
#include "runtime/gmm_helper/resource_info.h"
#include "igfxfmid.h"
#include <map>

namespace OCLRT {

ImageFuncs imageFactory[IGFX_MAX_CORE] = {};

Image::Image(Context *context,
             cl_mem_flags flags,
             size_t size,
             void *hostPtr,
             cl_image_format imageFormat,
             const cl_image_desc &imageDesc,
             bool zeroCopy,
             GraphicsAllocation *graphicsAllocation,
             bool isObjectRedescribed,
             bool createTiledImage,
             int mipLevel,
             const SurfaceFormatInfo *surfaceFormatInfo,
             const SurfaceOffsets *surfaceOffsets)
    : MemObj(context,
             imageDesc.image_type,
             flags,
             size,
             graphicsAllocation->getUnderlyingBuffer(),
             hostPtr,
             graphicsAllocation,
             zeroCopy,
             false,
             isObjectRedescribed),
      createFunction(nullptr),
      isTiledImage(createTiledImage),
      imageFormat(std::move(imageFormat)),
      imageDesc(imageDesc),
      surfaceFormatInfo(*surfaceFormatInfo),
      cubeFaceIndex(__GMM_NO_CUBE_MAP),
      mediaPlaneType(0),
      mipLevel(mipLevel) {
    magic = objectMagic;
    if (surfaceOffsets)
        setSurfaceOffsets(surfaceOffsets->offset, surfaceOffsets->xOffset, surfaceOffsets->yOffset, surfaceOffsets->yOffsetForUVplane);
    else
        setSurfaceOffsets(0, 0, 0, 0);
}

void Image::transferData(void *src, size_t srcRowPitch, size_t srcSlicePitch, void *dest, size_t destRowPitch, size_t destSlicePitch, cl_image_desc *imageDesc, size_t pixelSize, size_t imageCount) {
    size_t imageHeight = getValidParam(imageDesc->image_height);
    size_t imageDepth = getValidParam(imageDesc->image_depth);
    size_t lineWidth = getValidParam(imageDesc->image_width) * pixelSize;

    DBG_LOG(LogMemoryObject, __FUNCTION__, "memcpy dest:", dest, "sizeRowToCopy:", lineWidth, "src:", src);
    for (size_t count = 0; count < imageCount; count++) {
        for (size_t depth = 0; depth < imageDepth; ++depth) {
            auto currentImage = std::max(depth, count);
            auto srcPtr = ptrOffset(src, srcSlicePitch * currentImage);
            auto destPtr = ptrOffset(dest, destSlicePitch * currentImage);

            for (size_t height = 0; height < imageHeight; ++height) {
                memcpy_s(destPtr, lineWidth, srcPtr, lineWidth);
                srcPtr = ptrOffset(srcPtr, srcRowPitch);
                destPtr = ptrOffset(destPtr, destRowPitch);
            }
        }
    }
}

Image::~Image() = default;

Image *Image::create(Context *context,
                     cl_mem_flags flags,
                     const SurfaceFormatInfo *surfaceFormat,
                     const cl_image_desc *imageDesc,
                     const void *hostPtr,
                     cl_int &errcodeRet) {
    UNRECOVERABLE_IF(surfaceFormat == nullptr);
    Image *image = nullptr;
    GraphicsAllocation *memory = nullptr;
    const auto &hwInfo = context->getDevice(0)->getHardwareInfo();
    MemoryManager *memoryManager = context->getMemoryManager();
    Buffer *parentBuffer = castToObject<Buffer>(imageDesc->mem_object);
    Image *parentImage = castToObject<Image>(imageDesc->mem_object);

    do {
        size_t imageWidth = imageDesc->image_width;
        size_t imageHeight = 1;
        size_t imageDepth = 1;
        size_t imageCount = 1;
        size_t hostPtrMinSize = 0;

        cl_image_desc imageDescriptor = *imageDesc;
        ImageInfo imgInfo = {0};
        void *hostPtrToSet = nullptr;

        if (flags & CL_MEM_USE_HOST_PTR) {
            hostPtrToSet = const_cast<void *>(hostPtr);
        }

        imgInfo.imgDesc = &imageDescriptor;
        imgInfo.surfaceFormat = surfaceFormat;
        Gmm *gmm = nullptr;

        if (imageDesc->image_type == CL_MEM_OBJECT_IMAGE1D_ARRAY || imageDesc->image_type == CL_MEM_OBJECT_IMAGE2D_ARRAY) {
            imageCount = imageDesc->image_array_size;
        }

        switch (imageDesc->image_type) {
        case CL_MEM_OBJECT_IMAGE3D:
            imageDepth = imageDesc->image_depth;
        // FALLTHROUGH
        case CL_MEM_OBJECT_IMAGE2D:
        case CL_MEM_OBJECT_IMAGE2D_ARRAY:
            imageHeight = imageDesc->image_height;
        case CL_MEM_OBJECT_IMAGE1D:
        case CL_MEM_OBJECT_IMAGE1D_ARRAY:
        case CL_MEM_OBJECT_IMAGE1D_BUFFER:
            break;
        default:
            DEBUG_BREAK_IF("Unsupported cl_image_type");
            break;
        }

        if (parentImage) {
            DEBUG_BREAK_IF(!IsNV12Image(&parentImage->getImageFormat()));
            imageWidth = parentImage->getImageDesc().image_width;
            imageHeight = parentImage->getImageDesc().image_height;
            imageDepth = 1;

            if (imageDesc->image_depth == 1) { // UV Plane
                imageWidth /= 2;
                imageHeight /= 2;
                imgInfo.plane = GMM_PLANE_U;
            } else {
                imgInfo.plane = GMM_PLANE_Y;
            }

            imgInfo.surfaceFormat = &parentImage->surfaceFormatInfo;
            imageDescriptor = parentImage->getImageDesc();
        }

        auto hostPtrRowPitch = imageDesc->image_row_pitch ? imageDesc->image_row_pitch : imageWidth * surfaceFormat->ImageElementSizeInBytes;
        auto hostPtrSlicePitch = imageDesc->image_slice_pitch ? imageDesc->image_slice_pitch : hostPtrRowPitch * imageHeight;
        auto isTilingAllowed = context->isSharedContext ? false : Gmm::allowTiling(*imageDesc);
        imgInfo.preferRenderCompression = isTilingAllowed;

        bool zeroCopy = false;
        bool transferNeeded = false;
        bool imageRedescribed = false;
        bool copyRequired = false;
        if (((imageDesc->image_type == CL_MEM_OBJECT_IMAGE1D_BUFFER) || (imageDesc->image_type == CL_MEM_OBJECT_IMAGE2D)) && (parentBuffer != nullptr)) {
            imageRedescribed = true;
            memory = parentBuffer->getGraphicsAllocation();
            // Image from buffer - we never allocate memory, we use what buffer provides
            zeroCopy = true;
            hostPtr = parentBuffer->getHostPtr();
            hostPtrToSet = const_cast<void *>(hostPtr);
            parentBuffer->incRefInternal();
            Gmm::queryImgFromBufferParams(imgInfo, memory);
            if (memoryManager->peekVirtualPaddingSupport() && (imageDesc->image_type == CL_MEM_OBJECT_IMAGE2D)) {
                // Retrieve sizes from GMM and apply virtual padding if buffer storage is not big enough
                auto queryGmmImgInfo(imgInfo);
                std::unique_ptr<Gmm> gmm(Gmm::createGmmAndQueryImgParams(queryGmmImgInfo, hwInfo));
                auto gmmAllocationSize = gmm->gmmResourceInfo->getSizeAllocation();
                if (gmmAllocationSize > memory->getUnderlyingBufferSize()) {
                    memory = memoryManager->createGraphicsAllocationWithPadding(memory, gmmAllocationSize);
                }
            }
        }
        // NV12 image planes
        else if (parentImage != nullptr) {
            DEBUG_BREAK_IF(!IsNV12Image(&parentImage->getImageFormat()));
            memory = parentImage->getGraphicsAllocation();
            memory->gmm->queryImageParams(imgInfo, hwInfo);
            isTilingAllowed = parentImage->allowTiling();
        } else {
            gmm = new Gmm();
            gmm->queryImageParams(imgInfo, hwInfo);
            if (flags & CL_MEM_USE_HOST_PTR) {
                errcodeRet = CL_INVALID_HOST_PTR;
                if (hostPtr) {
                    size_t pointerPassedSize = hostPtrRowPitch * imageHeight * imageDepth * imageCount;
                    auto alignedSizePassedPointer = alignSizeWholePage(const_cast<void *>(hostPtr), pointerPassedSize);
                    auto alignedSizeRequiredForAllocation = alignSizeWholePage(const_cast<void *>(hostPtr), imgInfo.size);

                    // Passed pointer doesn't have enough memory, copy is needed
                    copyRequired = (alignedSizeRequiredForAllocation > alignedSizePassedPointer) |
                                   (imgInfo.rowPitch != hostPtrRowPitch) |
                                   (imgInfo.slicePitch != hostPtrSlicePitch) |
                                   ((reinterpret_cast<uintptr_t>(hostPtr) & (MemoryConstants::cacheLineSize - 1)) != 0) |
                                   isTilingAllowed;

                    if (copyRequired && !context->isSharedContext) {
                        errcodeRet = CL_OUT_OF_HOST_MEMORY;
                        memory = memoryManager->allocateGraphicsMemoryForImage(imgInfo, gmm);
                        zeroCopy = false;
                        transferNeeded = true;
                    } else {
                        // To avoid having two pointers in a MemObj we cast off the const here
                        // However, in USE_HOST_PTR cases we shouldn't be modifying the memory
                        memory = memoryManager->allocateGraphicsMemory(imgInfo.size, hostPtr);
                        memory->gmm = gmm;
                        zeroCopy = true;
                    }
                }
            } else {
                errcodeRet = CL_OUT_OF_HOST_MEMORY;
                memory = memoryManager->allocateGraphicsMemoryForImage(imgInfo, gmm);
                zeroCopy = true;
            }
        }

        switch (imageDesc->image_type) {
        case CL_MEM_OBJECT_IMAGE3D:
            hostPtrMinSize = hostPtrSlicePitch * imageDepth;
            break;
        case CL_MEM_OBJECT_IMAGE2D:
            if (IsNV12Image(&surfaceFormat->OCLImageFormat)) {
                hostPtrMinSize = hostPtrRowPitch * imageHeight + hostPtrRowPitch * imageHeight / 2;
            } else {
                hostPtrMinSize = hostPtrRowPitch * imageHeight;
            }
            hostPtrSlicePitch = 0;
            break;
        case CL_MEM_OBJECT_IMAGE1D_ARRAY:
        case CL_MEM_OBJECT_IMAGE2D_ARRAY:
            hostPtrMinSize = hostPtrSlicePitch * imageCount;
            break;
        case CL_MEM_OBJECT_IMAGE1D:
        case CL_MEM_OBJECT_IMAGE1D_BUFFER:
            hostPtrMinSize = hostPtrRowPitch;
            hostPtrSlicePitch = 0;
            break;
        default:
            DEBUG_BREAK_IF("Unsupported cl_image_type");
            break;
        }

        if (!memory) {
            if (gmm) {
                delete gmm;
            }
            break;
        }

        auto allocationType = (flags & (CL_MEM_READ_ONLY | CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS))
                                  ? GraphicsAllocation::ALLOCATION_TYPE_IMAGE
                                  : GraphicsAllocation::ALLOCATION_TYPE_IMAGE | GraphicsAllocation::ALLOCATION_TYPE_WRITABLE;
        memory->setAllocationType(allocationType);

        DBG_LOG(LogMemoryObject, __FUNCTION__, "hostPtr:", hostPtr, "size:", memory->getUnderlyingBufferSize(), "memoryStorage:", memory->getUnderlyingBuffer(), "GPU address:", std::hex, memory->getGpuAddress());

        if (!isTilingAllowed) {
            errcodeRet = CL_INVALID_VALUE;
            if (flags & CL_MEM_COPY_HOST_PTR || transferNeeded) {
                if (hostPtr) {
                    Image::transferData((void *)hostPtr, hostPtrRowPitch, hostPtrSlicePitch,
                                        memory->getUnderlyingBuffer(), imgInfo.rowPitch, imgInfo.slicePitch,
                                        (cl_image_desc *)imageDesc, surfaceFormat->ImageElementSizeInBytes, imageCount);
                } else {
                    memoryManager->freeGraphicsMemory(memory);
                    break;
                }
            }
        }
        if (parentImage) {
            imageDescriptor.image_height = imageHeight;
            imageDescriptor.image_width = imageWidth;
            imageDescriptor.image_type = CL_MEM_OBJECT_IMAGE2D;
            imageDescriptor.image_depth = 1;
            imageDescriptor.image_array_size = 0;
            imageDescriptor.image_row_pitch = 0;
            imageDescriptor.image_slice_pitch = 0;
            imageDescriptor.mem_object = imageDesc->mem_object;
            parentImage->incRefInternal();
        }

        image = createImageHw(context, flags, imgInfo.size, hostPtrToSet, surfaceFormat->OCLImageFormat,
                              imageDescriptor, zeroCopy, memory, imageRedescribed, isTilingAllowed, 0, surfaceFormat);

        if (imageDesc->image_type != CL_MEM_OBJECT_IMAGE1D_ARRAY && imageDesc->image_type != CL_MEM_OBJECT_IMAGE2D_ARRAY) {
            image->imageDesc.image_array_size = 0;
        }
        if ((imageDesc->image_type == CL_MEM_OBJECT_IMAGE1D_BUFFER) || ((imageDesc->image_type == CL_MEM_OBJECT_IMAGE2D) && (imageDesc->mem_object != nullptr))) {
            image->associatedMemObject = castToObject<MemObj>(imageDesc->mem_object);
        }
        // Driver needs to store rowPitch passed by the app in order to synchronize the host_ptr later on map call
        image->setHostPtrRowPitch(imageDesc->image_row_pitch ? imageDesc->image_row_pitch : hostPtrRowPitch);
        image->setHostPtrSlicePitch(hostPtrSlicePitch);
        image->setImageCount(imageCount);
        image->setHostPtrMinSize(hostPtrMinSize);
        image->setImageRowPitch(imgInfo.rowPitch);
        image->setImageSlicePitch(imgInfo.slicePitch);
        image->setQPitch(imgInfo.qPitch);
        image->setSurfaceOffsets(imgInfo.offset, imgInfo.xOffset, imgInfo.yOffset, imgInfo.yOffsetForUVPlane);
        if (parentImage) {
            image->setMediaPlaneType(static_cast<cl_uint>(imageDesc->image_depth));
            image->setParentSharingHandler(parentImage->getSharingHandler());
        }
        if (parentBuffer) {
            image->setParentSharingHandler(parentBuffer->getSharingHandler());
        }
        errcodeRet = CL_SUCCESS;

        if (isTilingAllowed) {
            if (flags & CL_MEM_COPY_HOST_PTR || transferNeeded) {
                if (!hostPtr) {
                    errcodeRet = CL_INVALID_VALUE;
                    image->release();
                    image = nullptr;
                    memory = nullptr;
                    break;
                }
                auto cmdQ = context->getSpecialQueue();

                size_t Origin[] = {0, 0, 0};
                size_t Region[] = {imageWidth, imageHeight, imageDepth};
                if (imageDesc->image_type == CL_MEM_OBJECT_IMAGE2D_ARRAY) {
                    Region[2] = imageDesc->image_array_size;
                }

                if (IsNV12Image(&image->getImageFormat())) {
                    errcodeRet = image->writeNV12Planes(hostPtr, hostPtrRowPitch);
                } else {
                    errcodeRet = cmdQ->enqueueWriteImage(image, CL_TRUE, Origin, Region,
                                                         hostPtrRowPitch, hostPtrSlicePitch,
                                                         hostPtr, 0, nullptr, nullptr);
                }
                if (errcodeRet != CL_SUCCESS) {
                    image->release();
                    image = nullptr;
                    memory = nullptr;
                    break;
                }
            }
        }

    } while (false);

    return image;
}

Image *Image::createImageHw(Context *context, cl_mem_flags flags, size_t size, void *hostPtr,
                            const cl_image_format &imageFormat, const cl_image_desc &imageDesc,
                            bool zeroCopy, GraphicsAllocation *graphicsAllocation,
                            bool isObjectRedescribed, bool createTiledImage, int mipLevel, const SurfaceFormatInfo *surfaceFormatInfo) {
    const auto device = castToObject<Context>(context)->getDevice(0);
    const auto &hwInfo = device->getHardwareInfo();

    auto funcCreate = imageFactory[hwInfo.pPlatform->eRenderCoreFamily].createImageFunction;
    DEBUG_BREAK_IF(nullptr == funcCreate);
    auto image = funcCreate(context, flags, size, hostPtr, imageFormat, imageDesc,
                            zeroCopy, graphicsAllocation, isObjectRedescribed, createTiledImage, mipLevel, surfaceFormatInfo, nullptr);
    DEBUG_BREAK_IF(nullptr == image);
    image->createFunction = funcCreate;
    return image;
}

Image *Image::createSharedImage(Context *context, SharingHandler *sharingHandler, McsSurfaceInfo &mcsSurfaceInfo,
                                GraphicsAllocation *graphicsAllocation, GraphicsAllocation *mcsAllocation,
                                cl_mem_flags flags, ImageInfo &imgInfo, uint32_t cubeFaceIndex, int mipLevel) {
    auto tileWalk = graphicsAllocation->gmm->gmmResourceInfo->getTileType();
    auto tileMode = Gmm::getRenderTileMode(tileWalk);
    bool isTiledImage = tileMode ? true : false;

    auto sharedImage = createImageHw(context, flags, graphicsAllocation->getUnderlyingBufferSize(),
                                     nullptr, imgInfo.surfaceFormat->OCLImageFormat, *imgInfo.imgDesc, false, graphicsAllocation, false, isTiledImage, mipLevel, imgInfo.surfaceFormat);
    sharedImage->setSharingHandler(sharingHandler);
    sharedImage->setMcsAllocation(mcsAllocation);
    sharedImage->setQPitch(imgInfo.qPitch);
    sharedImage->setHostPtrRowPitch(imgInfo.imgDesc->image_row_pitch);
    sharedImage->setHostPtrSlicePitch(imgInfo.imgDesc->image_slice_pitch);
    sharedImage->setCubeFaceIndex(cubeFaceIndex);
    sharedImage->setSurfaceOffsets(imgInfo.offset, imgInfo.xOffset, imgInfo.yOffset, imgInfo.yOffsetForUVPlane);
    sharedImage->setMcsSurfaceInfo(mcsSurfaceInfo);
    return sharedImage;
}

cl_int Image::unmapObj(CommandQueue *cmdQ, void *ptr,
                       cl_uint numEventsInWaitList,
                       const cl_event *eventWaitList,
                       cl_event *event) {
    if (!allowTiling() && !peekSharingHandler()) {
        return cmdQ->enqueueUnmapMemObject(this, ptr, numEventsInWaitList, eventWaitList, event);
    }

    if (ptr != getMappedPtr()) {
        return CL_INVALID_VALUE;
    }

    cl_int retVal;

    size_t Region[] = {mappedRegion[0] ? mappedRegion[0] : 1,
                       +mappedRegion[1] ? mappedRegion[1] : 1,
                       +mappedRegion[2] ? mappedRegion[2] : 1};

    size_t rowPitch = getHostPtrRowPitch();
    size_t slicePitch = getHostPtrSlicePitch();

    retVal = cmdQ->enqueueWriteImage(this,
                                     CL_FALSE, mappedOrigin, Region, rowPitch, slicePitch, getMappedPtr(),
                                     numEventsInWaitList,
                                     eventWaitList,
                                     event);
    bool mustCallFinish = true;
    if (!(flags & CL_MEM_USE_HOST_PTR)) {
        mustCallFinish = true;
    } else {
        mustCallFinish = (CommandQueue::getTaskLevelFromWaitList(cmdQ->taskLevel, numEventsInWaitList, eventWaitList) != Event::eventNotReady);
    }
    if (mustCallFinish) {
        cmdQ->finish(true);
    }
    return retVal;
}

cl_int Image::validate(Context *context,
                       cl_mem_flags flags,
                       const SurfaceFormatInfo *surfaceFormat,
                       const cl_image_desc *imageDesc,
                       const void *hostPtr) {
    auto pDevice = context->getDevice(0);
    cl_int retVal = CL_SUCCESS;
    size_t srcSize = 0;
    size_t retSize = 0;
    const size_t *maxWidth = nullptr;
    const size_t *maxHeight = nullptr;
    const uint32_t *pitchAlignment = nullptr;
    const uint32_t *baseAddressAlignment = nullptr;
    if (!surfaceFormat) {
        return CL_IMAGE_FORMAT_NOT_SUPPORTED;
    }
    switch (imageDesc->image_type) {

    case CL_MEM_OBJECT_IMAGE2D:
        pDevice->getCap<CL_DEVICE_IMAGE2D_MAX_WIDTH>(reinterpret_cast<const void *&>(maxWidth), srcSize, retSize);
        pDevice->getCap<CL_DEVICE_IMAGE2D_MAX_HEIGHT>(reinterpret_cast<const void *&>(maxHeight), srcSize, retSize);
        if (imageDesc->image_width > *maxWidth ||
            imageDesc->image_height > *maxHeight) {
            retVal = CL_INVALID_IMAGE_SIZE;
        }
        if (imageDesc->mem_object != nullptr) {
            // Image2d from buffer
            Buffer *inputBuffer = castToObject<Buffer>(imageDesc->mem_object);
            if (inputBuffer != nullptr) {
                pDevice->getCap<CL_DEVICE_IMAGE_PITCH_ALIGNMENT>(reinterpret_cast<const void *&>(pitchAlignment), srcSize, retSize);
                pDevice->getCap<CL_DEVICE_IMAGE_BASE_ADDRESS_ALIGNMENT>(reinterpret_cast<const void *&>(baseAddressAlignment), srcSize, retSize);

                if ((imageDesc->image_row_pitch % (*pitchAlignment)) ||
                    ((inputBuffer->getFlags() & CL_MEM_USE_HOST_PTR) && (reinterpret_cast<uint64_t>(inputBuffer->getHostPtr()) % (*baseAddressAlignment))) ||
                    (imageDesc->image_height * (imageDesc->image_row_pitch != 0 ? imageDesc->image_row_pitch : imageDesc->image_width) > inputBuffer->getSize())) {
                    retVal = CL_INVALID_IMAGE_FORMAT_DESCRIPTOR;
                } else if (flags & (CL_MEM_USE_HOST_PTR | CL_MEM_COPY_HOST_PTR)) {
                    retVal = CL_INVALID_VALUE;
                }
            }
        } else if (imageDesc->image_width == 0 ||
                   imageDesc->image_height == 0) {
            retVal = CL_INVALID_IMAGE_DESCRIPTOR;
        }
        break;
    default:
        break;
    }
    if (hostPtr == nullptr) {
        if (imageDesc->image_row_pitch != 0 && imageDesc->mem_object == nullptr) {
            retVal = CL_INVALID_IMAGE_DESCRIPTOR;
        }
    } else {
        if (imageDesc->image_row_pitch != 0) {
            if (imageDesc->image_row_pitch % surfaceFormat->ImageElementSizeInBytes != 0 ||
                imageDesc->image_row_pitch < imageDesc->image_width * surfaceFormat->ImageElementSizeInBytes) {
                retVal = CL_INVALID_IMAGE_DESCRIPTOR;
            }
        }
    }

    if ((imageDesc->mem_object != nullptr) && (imageDesc->image_type != CL_MEM_OBJECT_IMAGE1D_BUFFER) && (imageDesc->image_type != CL_MEM_OBJECT_IMAGE2D)) {
        retVal = CL_INVALID_IMAGE_FORMAT_DESCRIPTOR;
    }

    if (retVal != CL_SUCCESS) {
        return retVal;
    }

    retVal = validateImageTraits(context, flags, &surfaceFormat->OCLImageFormat, imageDesc, hostPtr);

    return retVal;
}

cl_int Image::validateImageFormat(const cl_image_format *imageFormat) {
    if (!imageFormat) {
        return CL_INVALID_IMAGE_FORMAT_DESCRIPTOR;
    }
    bool isValidFormat = isValidSingleChannelFormat(imageFormat) ||
                         isValidIntensityFormat(imageFormat) ||
                         isValidLuminanceFormat(imageFormat) ||
                         isValidDepthFormat(imageFormat) ||
                         isValidDoubleChannelFormat(imageFormat) ||
                         isValidTripleChannelFormat(imageFormat) ||
                         isValidRGBAFormat(imageFormat) ||
                         isValidSRGBFormat(imageFormat) ||
                         isValidARGBFormat(imageFormat) ||
                         isValidDepthStencilFormat(imageFormat);
#if SUPPORT_YUV
    isValidFormat = isValidFormat || isValidYUVFormat(imageFormat);
#endif
    if (isValidFormat) {
        return CL_SUCCESS;
    }
    return CL_INVALID_IMAGE_FORMAT_DESCRIPTOR;
}

cl_int Image::validatePlanarYUV(Context *context,
                                cl_mem_flags flags,
                                const cl_image_desc *imageDesc,
                                const void *hostPtr) {
    cl_int errorCode = CL_SUCCESS;
    auto pDevice = context->getDevice(0);
    const size_t *maxWidth = nullptr;
    const size_t *maxHeight = nullptr;
    size_t srcSize = 0;
    size_t retSize = 0;

    while (true) {

        Image *memObject = castToObject<Image>(imageDesc->mem_object);
        if (memObject != nullptr) {
            if (memObject->memObjectType == CL_MEM_OBJECT_IMAGE2D) {
                if (imageDesc->image_depth != 1 && imageDesc->image_depth != 0) {
                    errorCode = CL_INVALID_IMAGE_DESCRIPTOR;
                }
            }
            break;
        }

        if (imageDesc->mem_object != nullptr) {
            errorCode = CL_INVALID_IMAGE_DESCRIPTOR;
            break;
        }
        if (!(flags & CL_MEM_HOST_NO_ACCESS)) {
            errorCode = CL_INVALID_VALUE;
            break;
        } else {
            if (imageDesc->image_height % 4 ||
                imageDesc->image_width % 4 ||
                imageDesc->image_type != CL_MEM_OBJECT_IMAGE2D) {
                errorCode = CL_INVALID_IMAGE_DESCRIPTOR;
                break;
            }
        }

        pDevice->getCap<CL_DEVICE_PLANAR_YUV_MAX_WIDTH_INTEL>(reinterpret_cast<const void *&>(maxWidth), srcSize, retSize);
        pDevice->getCap<CL_DEVICE_PLANAR_YUV_MAX_HEIGHT_INTEL>(reinterpret_cast<const void *&>(maxHeight), srcSize, retSize);
        if (imageDesc->image_width > *maxWidth || imageDesc->image_height > *maxHeight) {
            errorCode = CL_INVALID_IMAGE_SIZE;
            break;
        }
        break;
    }
    return errorCode;
}

cl_int Image::validatePackedYUV(cl_mem_flags flags, const cl_image_desc *imageDesc) {
    cl_int errorCode = CL_SUCCESS;
    while (true) {
        if (!(flags & CL_MEM_READ_ONLY)) {
            errorCode = CL_INVALID_VALUE;
            break;
        } else {
            if (imageDesc->image_width % 2 != 0 ||
                imageDesc->image_type != CL_MEM_OBJECT_IMAGE2D) {
                errorCode = CL_INVALID_IMAGE_DESCRIPTOR;
                break;
            }
        }
        break;
    }
    return errorCode;
}

cl_int Image::validateImageTraits(Context *context, cl_mem_flags flags, const cl_image_format *imageFormat, const cl_image_desc *imageDesc, const void *hostPtr) {
    if (IsNV12Image(imageFormat))
        return validatePlanarYUV(context, flags, imageDesc, hostPtr);
    else if (IsPackedYuvImage(imageFormat))
        return validatePackedYUV(flags, imageDesc);

    return CL_SUCCESS;
}

size_t Image::calculateHostPtrSize(size_t *region, size_t rowPitch, size_t slicePitch, size_t pixelSize, uint32_t imageType) {
    DEBUG_BREAK_IF(!((rowPitch != 0) && (slicePitch != 0)));
    size_t sizeToReturn = 0u;

    switch (imageType) {
    case CL_MEM_OBJECT_IMAGE1D:
    case CL_MEM_OBJECT_IMAGE1D_BUFFER:
        sizeToReturn = region[0] * pixelSize;
        break;
    case CL_MEM_OBJECT_IMAGE2D:
        sizeToReturn = (region[1] - 1) * rowPitch + region[0] * pixelSize;
        break;
    case CL_MEM_OBJECT_IMAGE1D_ARRAY:
        sizeToReturn = (region[1] - 1) * slicePitch + region[0] * pixelSize;
        break;
    case CL_MEM_OBJECT_IMAGE3D:
    case CL_MEM_OBJECT_IMAGE2D_ARRAY:
        sizeToReturn = (region[2] - 1) * slicePitch + (region[1] - 1) * rowPitch + region[0] * pixelSize;
        break;
    default:
        DEBUG_BREAK_IF("Unsupported cl_image_type");
        break;
    }

    DEBUG_BREAK_IF(sizeToReturn == 0);
    return sizeToReturn;
}

// Called by clGetImageParamsINTEL to obtain image row pitch and slice pitch
// Assumption: all parameters are already validated be calling function
cl_int Image::getImageParams(Context *context,
                             cl_mem_flags memFlags,
                             const SurfaceFormatInfo *surfaceFormat,
                             const cl_image_desc *imageDesc,
                             size_t *imageRowPitch,
                             size_t *imageSlicePitch) {
    cl_int retVal = CL_SUCCESS;
    const auto &hwInfo = context->getDevice(0)->getHardwareInfo();

    ImageInfo imgInfo = {0};
    cl_image_desc imageDescriptor = *imageDesc;
    imgInfo.imgDesc = &imageDescriptor;
    imgInfo.surfaceFormat = surfaceFormat;

    Gmm *gmm = nullptr;
    gmm = new Gmm();
    gmm->queryImageParams(imgInfo, hwInfo);
    delete gmm;

    *imageRowPitch = imgInfo.rowPitch;
    *imageSlicePitch = imgInfo.slicePitch;

    return retVal;
}

const cl_image_desc &Image::getImageDesc() const {
    return imageDesc;
}

const cl_image_format &Image::getImageFormat() const {
    return imageFormat;
}

const SurfaceFormatInfo &Image::getSurfaceFormatInfo() const {
    return surfaceFormatInfo;
}

cl_int Image::getImageInfo(cl_image_info paramName,
                           size_t paramValueSize,
                           void *paramValue,
                           size_t *paramValueSizeRet) {
    cl_int retVal;
    size_t srcParamSize = 0;
    void *srcParam = nullptr;
    auto imageDesc = getImageDesc();
    auto surfFmtInfo = getSurfaceFormatInfo();
    size_t retParam;
    size_t array_size = imageDesc.image_array_size * (imageDesc.image_type == CL_MEM_OBJECT_IMAGE1D_ARRAY || imageDesc.image_type == CL_MEM_OBJECT_IMAGE2D_ARRAY);
    size_t SlicePitch = hostPtrSlicePitch * !(imageDesc.image_type == CL_MEM_OBJECT_IMAGE2D || imageDesc.image_type == CL_MEM_OBJECT_IMAGE1D || imageDesc.image_type == CL_MEM_OBJECT_IMAGE1D_BUFFER);

    switch (paramName) {
    case CL_IMAGE_FORMAT:
        srcParamSize = sizeof(cl_image_format);
        srcParam = &(surfFmtInfo.OCLImageFormat);
        break;

    case CL_IMAGE_ELEMENT_SIZE:
        srcParamSize = sizeof(size_t);
        srcParam = &(surfFmtInfo.ImageElementSizeInBytes);
        break;

    case CL_IMAGE_ROW_PITCH:
        srcParamSize = sizeof(size_t);
        srcParam = &hostPtrRowPitch;
        break;

    case CL_IMAGE_SLICE_PITCH:
        srcParamSize = sizeof(size_t);
        srcParam = &SlicePitch;
        break;

    case CL_IMAGE_WIDTH:
        srcParamSize = sizeof(size_t);
        retParam = imageDesc.image_width;
        if (this->mipLevel) {
            retParam = imageDesc.image_width >> this->mipLevel;
            retParam = std::max(retParam, (size_t)1);
        }
        srcParam = &retParam;
        break;

    case CL_IMAGE_HEIGHT:
        srcParamSize = sizeof(size_t);
        retParam = imageDesc.image_height * !((imageDesc.image_type == CL_MEM_OBJECT_IMAGE1D) || (imageDesc.image_type == CL_MEM_OBJECT_IMAGE1D_ARRAY) || (imageDesc.image_type == CL_MEM_OBJECT_IMAGE1D_BUFFER));
        if ((retParam != 0) && (this->mipLevel > 0)) {
            retParam = retParam >> this->mipLevel;
            retParam = std::max(retParam, (size_t)1);
        }
        srcParam = &retParam;
        break;

    case CL_IMAGE_DEPTH:
        srcParamSize = sizeof(size_t);
        retParam = imageDesc.image_depth * (imageDesc.image_type == CL_MEM_OBJECT_IMAGE3D);
        if ((retParam != 0) && (this->mipLevel > 0)) {
            retParam = retParam >> this->mipLevel;
            retParam = std::max(retParam, (size_t)1);
        }
        srcParam = &retParam;
        break;

    case CL_IMAGE_ARRAY_SIZE:
        srcParamSize = sizeof(size_t);
        srcParam = &(array_size);
        break;

    case CL_IMAGE_BUFFER:
        srcParamSize = sizeof(cl_mem);
        srcParam = &(imageDesc.buffer);
        break;

    case CL_IMAGE_NUM_MIP_LEVELS:
        srcParamSize = sizeof(cl_uint);
        srcParam = &(imageDesc.num_mip_levels);
        break;

    case CL_IMAGE_NUM_SAMPLES:
        srcParamSize = sizeof(cl_uint);
        srcParam = &(imageDesc.num_samples);
        break;

    default:
        getOsSpecificImageInfo(paramName, &srcParamSize, &srcParam);
        break;
    }

    retVal = ::getInfo(paramValue, paramValueSize, srcParam, srcParamSize);

    if (paramValueSizeRet) {
        *paramValueSizeRet = srcParamSize;
    }

    return retVal;
}

Image *Image::redescribeFillImage() {
    const uint32_t redescribeTable[3][3] = {
        {17, 27, 5}, // {CL_R, CL_UNSIGNED_INT8},  {CL_RG, CL_UNSIGNED_INT8},  {CL_RGBA, CL_UNSIGNED_INT8}
        {18, 28, 6}, // {CL_R, CL_UNSIGNED_INT16}, {CL_RG, CL_UNSIGNED_INT16}, {CL_RGBA, CL_UNSIGNED_INT16}
        {19, 29, 7}  // {CL_R, CL_UNSIGNED_INT32}, {CL_RG, CL_UNSIGNED_INT32}, {CL_RGBA, CL_UNSIGNED_INT32}
    };

    auto imageFormatNew = this->imageFormat;
    auto imageDescNew = this->imageDesc;
    const SurfaceFormatInfo *surfaceFormat = nullptr;
    uint32_t redescribeTableCol = this->surfaceFormatInfo.NumChannels / 2;
    uint32_t redescribeTableRow = this->surfaceFormatInfo.PerChannelSizeInBytes / 2;

    uint32_t surfaceFormatIdx = redescribeTable[redescribeTableRow][redescribeTableCol];
    surfaceFormat = &readWriteSurfaceFormats[surfaceFormatIdx];

    imageFormatNew.image_channel_order = surfaceFormat->OCLImageFormat.image_channel_order;
    imageFormatNew.image_channel_data_type = surfaceFormat->OCLImageFormat.image_channel_data_type;

    DEBUG_BREAK_IF(nullptr == createFunction);
    auto image = createFunction(context,
                                flags | CL_MEM_USE_HOST_PTR,
                                this->getSize(),
                                this->getCpuAddress(),
                                imageFormatNew,
                                imageDescNew,
                                this->isMemObjZeroCopy(),
                                this->getGraphicsAllocation(),
                                true,
                                isTiledImage,
                                this->mipLevel,
                                surfaceFormat,
                                &this->surfaceOffsets);
    image->setQPitch(this->getQPitch());
    image->setCubeFaceIndex(this->getCubeFaceIndex());
    return image;
}

Image *Image::redescribe() {

    const uint32_t redescribeTableBytes[] = {
        17, // {CL_R, CL_UNSIGNED_INT8}        1 byte
        18, // {CL_R, CL_UNSIGNED_INT16}       2 byte
        19, // {CL_R, CL_UNSIGNED_INT32}       4 byte
        29, // {CL_RG, CL_UNSIGNED_INT32}      8 byte
        7   // {CL_RGBA, CL_UNSIGNED_INT32}    16 byte
    };

    auto imageFormatNew = this->imageFormat;
    auto imageDescNew = this->imageDesc;
    const SurfaceFormatInfo *surfaceFormat = nullptr;
    auto bytesPerPixel = this->surfaceFormatInfo.NumChannels * surfaceFormatInfo.PerChannelSizeInBytes;
    uint32_t exponent = 0;

    exponent = Math::log2(bytesPerPixel);
    DEBUG_BREAK_IF(exponent >= 32);

    uint32_t surfaceFormatIdx = redescribeTableBytes[exponent % 5];
    surfaceFormat = &readWriteSurfaceFormats[surfaceFormatIdx];

    imageFormatNew.image_channel_order = surfaceFormat->OCLImageFormat.image_channel_order;
    imageFormatNew.image_channel_data_type = surfaceFormat->OCLImageFormat.image_channel_data_type;

    DEBUG_BREAK_IF(nullptr == createFunction);
    auto image = createFunction(context,
                                flags | CL_MEM_USE_HOST_PTR,
                                this->getSize(),
                                this->getCpuAddress(),
                                imageFormatNew,
                                imageDescNew,
                                this->isMemObjZeroCopy(),
                                this->getGraphicsAllocation(),
                                true,
                                isTiledImage,
                                this->mipLevel,
                                surfaceFormat,
                                &this->surfaceOffsets);
    image->setQPitch(this->getQPitch());
    image->setCubeFaceIndex(this->getCubeFaceIndex());
    return image;
}

void *Image::transferDataToHostPtr() {
    Image::transferData(graphicsAllocation->getUnderlyingBuffer(), imageDesc.image_row_pitch, imageDesc.image_slice_pitch,
                        hostPtr, hostPtrRowPitch, hostPtrSlicePitch, &imageDesc, surfaceFormatInfo.ImageElementSizeInBytes, imageCount);
    return hostPtr;
}

void Image::transferDataFromHostPtrToMemoryStorage() {
    Image::transferData(hostPtr, hostPtrRowPitch, hostPtrSlicePitch,
                        memoryStorage, imageDesc.image_row_pitch, imageDesc.image_slice_pitch,
                        &imageDesc, surfaceFormatInfo.ImageElementSizeInBytes, imageCount);
}

cl_int Image::writeNV12Planes(const void *hostPtr, size_t hostPtrRowPitch) {
    CommandQueue *cmdQ = context->getSpecialQueue();
    size_t origin[3] = {0, 0, 0};
    size_t region[3] = {this->imageDesc.image_width, this->imageDesc.image_height, 1};

    cl_int retVal = 0;
    cl_image_desc imageDesc = {0};
    cl_image_format imageFormat = {0};
    // Make NV12 planes readable and writable both on device and host
    cl_mem_flags flags = CL_MEM_READ_WRITE;

    // Plane Y
    imageFormat.image_channel_data_type = CL_UNORM_INT8;
    imageFormat.image_channel_order = CL_R;

    imageDesc.image_type = CL_MEM_OBJECT_IMAGE2D;
    // image_width & image_height are ignored for plane extraction
    imageDesc.image_width = 0;
    imageDesc.image_height = 0;
    // set mem_object to the full NV12 image
    imageDesc.mem_object = this;
    // get access to the Y plane (CL_R)
    imageDesc.image_depth = 0;
    SurfaceFormatInfo *surfaceFormat = (SurfaceFormatInfo *)Image::getSurfaceFormatFromTable(flags, &imageFormat);

    // Create NV12 UV Plane image
    std::unique_ptr<Image> imageYPlane(Image::create(
        context,
        flags,
        surfaceFormat,
        &imageDesc,
        nullptr,
        retVal));

    retVal = cmdQ->enqueueWriteImage(imageYPlane.get(), CL_TRUE, origin, region, hostPtrRowPitch, 0, hostPtr, 0, nullptr, nullptr);

    // UV Plane is two times smaller than Plane Y
    region[0] = region[0] / 2;
    region[1] = region[1] / 2;

    imageDesc.image_width = 0;
    imageDesc.image_height = 0;
    imageDesc.image_depth = 1; // UV plane
    imageFormat.image_channel_order = CL_RG;

    hostPtr = static_cast<const void *>(static_cast<const char *>(hostPtr) + (hostPtrRowPitch * this->imageDesc.image_height));
    surfaceFormat = (SurfaceFormatInfo *)Image::getSurfaceFormatFromTable(flags, &imageFormat);
    // Create NV12 UV Plane image
    std::unique_ptr<Image> imageUVPlane(Image::create(
        context,
        flags,
        surfaceFormat,
        &imageDesc,
        nullptr,
        retVal));

    retVal = cmdQ->enqueueWriteImage(imageUVPlane.get(), CL_TRUE, origin, region, hostPtrRowPitch, 0, hostPtr, 0, nullptr, nullptr);

    return retVal;
}

const SurfaceFormatInfo *Image::getSurfaceFormatFromTable(cl_mem_flags flags, const cl_image_format *imageFormat) {
    if (!imageFormat) {
        return nullptr;
    }
    const SurfaceFormatInfo *surfaceFormatTable = nullptr;
    size_t numSurfaceFormats = 0;
    bool isDepthFormat = Image::isDepthFormat(*imageFormat);

    if (IsNV12Image(imageFormat)) {
#if SUPPORT_YUV
        surfaceFormatTable = planarYuvSurfaceFormats;
        numSurfaceFormats = numPlanarYuvSurfaceFormats;
#else
        return nullptr;
#endif
    } else if (IsPackedYuvImage(imageFormat)) {
#if SUPPORT_YUV
        surfaceFormatTable = packedYuvSurfaceFormats;
        numSurfaceFormats = numPackedYuvSurfaceFormats;
#else
        return nullptr;
#endif
    } else if (isSnormFormat(*imageFormat)) {
        surfaceFormatTable = snormSurfaceFormats;
        numSurfaceFormats = numSnormSurfaceFormats;
    } else if ((flags & CL_MEM_READ_ONLY) == CL_MEM_READ_ONLY) {
        surfaceFormatTable = isDepthFormat ? readOnlyDepthSurfaceFormats : readOnlySurfaceFormats;
        numSurfaceFormats = isDepthFormat ? numReadOnlyDepthSurfaceFormats : numReadOnlySurfaceFormats;
    } else if ((flags & CL_MEM_WRITE_ONLY) == CL_MEM_WRITE_ONLY) {
        surfaceFormatTable = isDepthFormat ? readWriteDepthSurfaceFormats : writeOnlySurfaceFormats;
        numSurfaceFormats = isDepthFormat ? numReadWriteDepthSurfaceFormats : numWriteOnlySurfaceFormats;
    } else {
        surfaceFormatTable = isDepthFormat ? readWriteDepthSurfaceFormats : readWriteSurfaceFormats;
        numSurfaceFormats = isDepthFormat ? numReadWriteDepthSurfaceFormats : numReadWriteSurfaceFormats;
    }

    // Find a matching surface format
    size_t indexSurfaceFormat = 0;
    while (indexSurfaceFormat < numSurfaceFormats) {
        const auto &surfaceFormat = surfaceFormatTable[indexSurfaceFormat].OCLImageFormat;
        if (surfaceFormat.image_channel_data_type == imageFormat->image_channel_data_type &&
            surfaceFormat.image_channel_order == imageFormat->image_channel_order) {
            break;
        }
        ++indexSurfaceFormat;
    }

    if (indexSurfaceFormat >= numSurfaceFormats) {
        return nullptr;
    }

    return &surfaceFormatTable[indexSurfaceFormat];
}

bool Image::isImage2d(cl_mem_object_type imageType) {
    return (imageType == CL_MEM_OBJECT_IMAGE2D);
}

bool Image::isImage2dOr2dArray(cl_mem_object_type imageType) {
    return (imageType == CL_MEM_OBJECT_IMAGE2D) || (imageType == CL_MEM_OBJECT_IMAGE2D_ARRAY);
}

bool Image::isDepthFormat(const cl_image_format &imageFormat) {
    if (imageFormat.image_channel_order == CL_DEPTH || imageFormat.image_channel_order == CL_DEPTH_STENCIL) {
        return true;
    }
    return false;
}

bool Image::isSnormFormat(const cl_image_format &imageFormat) {
    if (imageFormat.image_channel_data_type == CL_SNORM_INT8 || imageFormat.image_channel_data_type == CL_SNORM_INT16) {
        return true;
    }
    return false;
}

Image *Image::validateAndCreateImage(Context *context,
                                     cl_mem_flags flags,
                                     const cl_image_format *imageFormat,
                                     const cl_image_desc *imageDesc,
                                     const void *hostPtr,
                                     cl_int &errcodeRet) {
    if (errcodeRet != CL_SUCCESS) {
        return nullptr;
    }
    SurfaceFormatInfo *surfaceFormat = nullptr;
    Image *image = nullptr;
    do {
        errcodeRet = Image::validateImageFormat(imageFormat);
        if (CL_SUCCESS != errcodeRet) {
            break;
        }
        surfaceFormat = (SurfaceFormatInfo *)Image::getSurfaceFormatFromTable(flags, imageFormat);
        errcodeRet = Image::validate(context, flags, surfaceFormat, imageDesc, hostPtr);
        if (CL_SUCCESS != errcodeRet) {
            break;
        }
        image = Image::create(context, flags, surfaceFormat, imageDesc, hostPtr, errcodeRet);
    } while (false);
    return image;
}

bool Image::isValidSingleChannelFormat(const cl_image_format *imageFormat) {
    auto channelOrder = imageFormat->image_channel_order;
    auto dataType = imageFormat->image_channel_data_type;

    bool isValidOrder = (channelOrder == CL_A) ||
                        (channelOrder == CL_R) ||
                        (channelOrder == CL_Rx);

    bool isValidDataType = (dataType == CL_UNORM_INT8) ||
                           (dataType == CL_UNORM_INT16) ||
                           (dataType == CL_SNORM_INT8) ||
                           (dataType == CL_SNORM_INT16) ||
                           (dataType == CL_HALF_FLOAT) ||
                           (dataType == CL_FLOAT) ||
                           (dataType == CL_SIGNED_INT8) ||
                           (dataType == CL_SIGNED_INT16) ||
                           (dataType == CL_SIGNED_INT32) ||
                           (dataType == CL_UNSIGNED_INT8) ||
                           (dataType == CL_UNSIGNED_INT16) ||
                           (dataType == CL_UNSIGNED_INT32);

    return isValidOrder && isValidDataType;
}

bool Image::isValidIntensityFormat(const cl_image_format *imageFormat) {
    if (imageFormat->image_channel_order != CL_INTENSITY) {
        return false;
    }
    auto dataType = imageFormat->image_channel_data_type;
    return (dataType == CL_UNORM_INT8) ||
           (dataType == CL_UNORM_INT16) ||
           (dataType == CL_SNORM_INT8) ||
           (dataType == CL_SNORM_INT16) ||
           (dataType == CL_HALF_FLOAT) ||
           (dataType == CL_FLOAT);
}

bool Image::isValidLuminanceFormat(const cl_image_format *imageFormat) {
    if (imageFormat->image_channel_order != CL_LUMINANCE) {
        return false;
    }
    auto dataType = imageFormat->image_channel_data_type;
    return (dataType == CL_UNORM_INT8) ||
           (dataType == CL_UNORM_INT16) ||
           (dataType == CL_SNORM_INT8) ||
           (dataType == CL_SNORM_INT16) ||
           (dataType == CL_HALF_FLOAT) ||
           (dataType == CL_FLOAT);
}

bool Image::isValidDepthFormat(const cl_image_format *imageFormat) {
    if (imageFormat->image_channel_order != CL_DEPTH) {
        return false;
    }
    auto dataType = imageFormat->image_channel_data_type;
    return (dataType == CL_UNORM_INT16) ||
           (dataType == CL_FLOAT);
}

bool Image::isValidDoubleChannelFormat(const cl_image_format *imageFormat) {
    auto channelOrder = imageFormat->image_channel_order;
    auto dataType = imageFormat->image_channel_data_type;

    bool isValidOrder = (channelOrder == CL_RG) ||
                        (channelOrder == CL_RGx) ||
                        (channelOrder == CL_RA);

    bool isValidDataType = (dataType == CL_UNORM_INT8) ||
                           (dataType == CL_UNORM_INT16) ||
                           (dataType == CL_SNORM_INT8) ||
                           (dataType == CL_SNORM_INT16) ||
                           (dataType == CL_HALF_FLOAT) ||
                           (dataType == CL_FLOAT) ||
                           (dataType == CL_SIGNED_INT8) ||
                           (dataType == CL_SIGNED_INT16) ||
                           (dataType == CL_SIGNED_INT32) ||
                           (dataType == CL_UNSIGNED_INT8) ||
                           (dataType == CL_UNSIGNED_INT16) ||
                           (dataType == CL_UNSIGNED_INT32);

    return isValidOrder && isValidDataType;
}

bool Image::isValidTripleChannelFormat(const cl_image_format *imageFormat) {
    auto channelOrder = imageFormat->image_channel_order;
    auto dataType = imageFormat->image_channel_data_type;

    bool isValidOrder = (channelOrder == CL_RGB) ||
                        (channelOrder == CL_RGBx);

    bool isValidDataType = (dataType == CL_UNORM_SHORT_565) ||
                           (dataType == CL_UNORM_SHORT_555) ||
                           (dataType == CL_UNORM_INT_101010);

    return isValidOrder && isValidDataType;
}

bool Image::isValidRGBAFormat(const cl_image_format *imageFormat) {
    if (imageFormat->image_channel_order != CL_RGBA) {
        return false;
    }
    auto dataType = imageFormat->image_channel_data_type;
    return (dataType == CL_UNORM_INT8) ||
           (dataType == CL_UNORM_INT16) ||
           (dataType == CL_SNORM_INT8) ||
           (dataType == CL_SNORM_INT16) ||
           (dataType == CL_HALF_FLOAT) ||
           (dataType == CL_FLOAT) ||
           (dataType == CL_SIGNED_INT8) ||
           (dataType == CL_SIGNED_INT16) ||
           (dataType == CL_SIGNED_INT32) ||
           (dataType == CL_UNSIGNED_INT8) ||
           (dataType == CL_UNSIGNED_INT16) ||
           (dataType == CL_UNSIGNED_INT32);
}

bool Image::isValidSRGBFormat(const cl_image_format *imageFormat) {
    auto channelOrder = imageFormat->image_channel_order;
    auto dataType = imageFormat->image_channel_data_type;

    bool isValidOrder = (channelOrder == CL_sRGB) ||
                        (channelOrder == CL_sRGBx) ||
                        (channelOrder == CL_sRGBA) ||
                        (channelOrder == CL_sBGRA);

    bool isValidDataType = (dataType == CL_UNORM_INT8);

    return isValidOrder && isValidDataType;
}

bool Image::isValidARGBFormat(const cl_image_format *imageFormat) {
    auto channelOrder = imageFormat->image_channel_order;
    auto dataType = imageFormat->image_channel_data_type;

    bool isValidOrder = (channelOrder == CL_ARGB) ||
                        (channelOrder == CL_BGRA) ||
                        (channelOrder == CL_ABGR);

    bool isValidDataType = (dataType == CL_UNORM_INT8) ||
                           (dataType == CL_SNORM_INT8) ||
                           (dataType == CL_SIGNED_INT8) ||
                           (dataType == CL_UNSIGNED_INT8);

    return isValidOrder && isValidDataType;
}

bool Image::isValidDepthStencilFormat(const cl_image_format *imageFormat) {
    if (imageFormat->image_channel_order != CL_DEPTH_STENCIL) {
        return false;
    }
    auto dataType = imageFormat->image_channel_data_type;
    return (dataType == CL_UNORM_INT24) ||
           (dataType == CL_FLOAT);
}

bool Image::isValidYUVFormat(const cl_image_format *imageFormat) {
    auto dataType = imageFormat->image_channel_data_type;

    bool isValidOrder = IsNV12Image(imageFormat) || IsPackedYuvImage(imageFormat);

    bool isValidDataType = (dataType == CL_UNORM_INT8);

    return isValidOrder && isValidDataType;
}

bool Image::hasAlphaChannel(const cl_image_format *imageFormat) {
    auto channelOrder = imageFormat->image_channel_order;
    return (channelOrder == CL_A) ||
           (channelOrder == CL_Rx) ||
           (channelOrder == CL_RA) ||
           (channelOrder == CL_RGx) ||
           (channelOrder == CL_RGBx) ||
           (channelOrder == CL_RGBA) ||
           (channelOrder == CL_BGRA) ||
           (channelOrder == CL_ARGB) ||
           (channelOrder == CL_INTENSITY) ||
           (channelOrder == CL_sRGBA) ||
           (channelOrder == CL_sBGRA) ||
           (channelOrder == CL_sRGBx) ||
           (channelOrder == CL_ABGR);
}
} // namespace OCLRT

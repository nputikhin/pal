/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2015-2022 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/

#include "core/hw/gfxip/borderColorPalette.h"
#include "core/hw/gfxip/gfxCmdBuffer.h"
#include "core/hw/gfxip/gfxDevice.h"
#include "core/hw/gfxip/computePipeline.h"
#include "core/hw/gfxip/rpm/rsrcProcMgr.h"
#include "palAutoBuffer.h"
#include "palHsaAbiMetadata.h"

using namespace Util;

namespace Pal
{

// =====================================================================================================================
GfxCmdBuffer::GfxCmdBuffer(
    const GfxDevice&           device,
    const CmdBufferCreateInfo& createInfo)
    :
    CmdBuffer(*device.Parent(), createInfo),
    m_engineSupport(0),
    m_computeState{},
    m_computeRestoreState{},
    m_gfxCmdBufStateFlags{},
    m_generatedChunkList(device.GetPlatform()),
    m_retainedGeneratedChunkList(device.GetPlatform()),
    m_pCurrentExperiment(nullptr),
    m_gfxIpLevel(device.Parent()->ChipProperties().gfxLevel),
    m_maxUploadFenceToken(0),
    m_device(device),
    m_pInternalEvent(nullptr),
    m_timestampGpuVa(0),
    m_computeStateFlags(0),
    m_pDfSpmPerfmonInfo(nullptr),
    m_cmdBufPerfExptFlags{}
{
    PAL_ASSERT((createInfo.queueType == QueueTypeUniversal) || (createInfo.queueType == QueueTypeCompute));
}

// =====================================================================================================================
GfxCmdBuffer::~GfxCmdBuffer()
{
    ReturnGeneratedCommandChunks(true);

    Device* pDevice = m_device.Parent();

    if (m_pInternalEvent != nullptr)
    {
        m_pInternalEvent->Destroy();
        PAL_SAFE_FREE(m_pInternalEvent, pDevice->GetPlatform());
    }

    PAL_SAFE_FREE(m_computeState.pKernelArguments, pDevice->GetPlatform());
    PAL_SAFE_FREE(m_computeRestoreState.pKernelArguments, pDevice->GetPlatform());
}

// =====================================================================================================================
Result GfxCmdBuffer::Init(
    const CmdBufferInternalCreateInfo& internalInfo)
{
    Result result = CmdBuffer::Init(internalInfo);

    Device* pDevice = m_device.Parent();

    if (result == Result::Success)
    {
        // Create gpuEvent for this CmdBuffer.
        GpuEventCreateInfo createInfo  = {};
        createInfo.flags.gpuAccessOnly = 1;

        const size_t eventSize = pDevice->GetGpuEventSize(createInfo, &result);

        if (result == Result::Success)
        {
            result = Result::ErrorOutOfMemory;
            void* pMemory = PAL_MALLOC(eventSize, pDevice->GetPlatform(), Util::SystemAllocType::AllocObject);

            if (pMemory != nullptr)
            {
                result = pDevice->CreateGpuEvent(createInfo,
                                                 pMemory,
                                                 reinterpret_cast<IGpuEvent**>(&m_pInternalEvent));
                if (result != Result::Success)
                {
                    PAL_SAFE_FREE(pMemory, pDevice->GetPlatform());
                }
            }
        }
    }

    return result;
}

// =====================================================================================================================
Result GfxCmdBuffer::Reset(
    ICmdAllocator* pCmdAllocator,
    bool           returnGpuMemory)
{
    // Do this before our parent class changes the allocator.
    ReturnGeneratedCommandChunks(returnGpuMemory);

    return CmdBuffer::Reset(pCmdAllocator, returnGpuMemory);
}

// =====================================================================================================================
// Completes recording of a command buffer in the building state, making it executable.
// Also ends command buffer dumping, if it is enabled.
Result GfxCmdBuffer::End()
{
    Result result = CmdBuffer::End();

    // NOTE: The root chunk comes from the last command stream in this command buffer because for universal command
    // buffers, the order of command streams is CE, DE. We always want the "DE" to be the root since the CE may not
    // have any commands, depending on what operations get recorded to the command buffer.
    CmdStreamChunk* pRootChunk = GetCmdStream(NumCmdStreams() - 1)->GetFirstChunk();

    // Finalize all generated command chunks.
    for (auto iter = m_generatedChunkList.Begin(); iter.IsValid(); iter.Next())
    {
        CmdStreamChunk* pChunk = iter.Get();
        pChunk->UpdateRootInfo(pRootChunk);
        pChunk->FinalizeCommands();
    }

    return result;
}

// =====================================================================================================================
void GfxCmdBuffer::ResetState()
{
    CmdBuffer::ResetState();

    PAL_SAFE_FREE(m_computeState.pKernelArguments, m_device.GetPlatform());
    memset(&m_computeState, 0, sizeof(m_computeState));

    PAL_SAFE_FREE(m_computeRestoreState.pKernelArguments, m_device.GetPlatform());
    memset(&m_computeRestoreState, 0, sizeof(m_computeRestoreState));

    m_gfxCmdBufStateFlags.u32All = 0;
}

// =====================================================================================================================
void GfxCmdBuffer::DescribeDispatch(
    Developer::DrawDispatchType cmdType,
    uint32                      xDim,
    uint32                      yDim,
    uint32                      zDim)
{
    PAL_ASSERT((cmdType == Developer::DrawDispatchType::CmdDispatch)        ||
               (cmdType == Developer::DrawDispatchType::CmdDispatchDynamic) ||
               (cmdType == Developer::DrawDispatchType::CmdDispatchAce));

    RgpMarkerSubQueueFlags subQueueFlags { };
    if (cmdType == Developer::DrawDispatchType::CmdDispatchAce)
    {
        subQueueFlags.includeGangedSubQueues = 1;
    }
    else
    {
        subQueueFlags.includeMainSubQueue = 1;
    }

    m_device.DescribeDispatch(this, subQueueFlags, cmdType, 0, 0, 0, xDim, yDim, zDim);
}

// =====================================================================================================================
void GfxCmdBuffer::DescribeDispatchOffset(
    uint32 xOffset,
    uint32 yOffset,
    uint32 zOffset,
    uint32 xDim,
    uint32 yDim,
    uint32 zDim)
{
    RgpMarkerSubQueueFlags subQueueFlags { };
    subQueueFlags.includeMainSubQueue = 1;

    m_device.DescribeDispatch(this,
        subQueueFlags,
        Developer::DrawDispatchType::CmdDispatchOffset,
        xOffset,
        yOffset,
        zOffset,
        xDim,
        yDim,
        zDim);
}

// =====================================================================================================================
void GfxCmdBuffer::DescribeDispatchIndirect()
{
    RgpMarkerSubQueueFlags subQueueFlags { };
    subQueueFlags.includeMainSubQueue = 1;

    m_device.DescribeDispatch(this, subQueueFlags, Developer::DrawDispatchType::CmdDispatchIndirect, 0, 0, 0, 0, 0, 0);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdBindPipeline(
    const PipelineBindParams& params)
{
    // Try to catch users who try to bind graphics pipelines to compute command buffers.
    PAL_ASSERT((params.pipelineBindPoint == PipelineBindPoint::Compute) || IsGraphicsSupported());

    const auto*const pPipeline = static_cast<const Pipeline*>(params.pPipeline);

    if (params.pipelineBindPoint == PipelineBindPoint::Compute)
    {
        m_computeState.pipelineState.pPipeline  = pPipeline;
        m_computeState.pipelineState.apiPsoHash = params.apiPsoHash;
        m_computeState.pipelineState.dirtyFlags.pipelineDirty = 1;

        m_computeState.dynamicCsInfo = params.cs;
        m_computeState.hsaAbiMode    = (pPipeline != nullptr) && (pPipeline->GetInfo().flags.hsaAbi == 1);

        // It's simplest to always free the kernel args buffer and allocate a new one with the proper size if needed.
        PAL_SAFE_FREE(m_computeState.pKernelArguments, m_device.GetPlatform());

        if (m_computeState.hsaAbiMode)
        {
            // HSA mode overwrites the user-data SGPRs. The easiest way to force user-data validation when we return
            // to PAL mode is to mark all user-data values that have ever been set as dirty.
            memcpy(m_computeState.csUserDataEntries.dirty,
                   m_computeState.csUserDataEntries.touched,
                   sizeof(m_computeState.csUserDataEntries.dirty));

            const ComputePipeline&            pipeline = *static_cast<const ComputePipeline*>(pPipeline);
            const HsaAbi::CodeObjectMetadata& metadata = pipeline.HsaMetadata();

            // We're callocing here on purpose because some HSA ABI arguments need to use zero by default.
            m_computeState.pKernelArguments = static_cast<uint8*>(PAL_CALLOC(metadata.KernargSegmentSize(),
                                                                             m_device.GetPlatform(),
                                                                             AllocInternal));

            if (m_computeState.pKernelArguments == nullptr)
            {
                // Allocation failure, mark buffer as faulty.
                NotifyAllocFailure();
            }
        }
    }

    m_device.DescribeBindPipeline(this, pPipeline, params.apiPsoHash, params.pipelineBindPoint);

    if (pPipeline != nullptr)
    {
        m_maxUploadFenceToken = Max(m_maxUploadFenceToken, pPipeline->GetUploadFenceToken());
        m_lastPagingFence     = Max(m_lastPagingFence,     pPipeline->GetPagingFenceVal());
    }
}

// =====================================================================================================================
// Puts command stream related objects into a state ready for command building.
Result GfxCmdBuffer::BeginCommandStreams(
    CmdStreamBeginFlags cmdStreamFlags,
    bool                doReset)
{
    if (doReset)
    {
        ReturnGeneratedCommandChunks(true);
    }

    Result result = CmdBuffer::BeginCommandStreams(cmdStreamFlags, doReset);

    if (result == Result::Success)
    {
        // Allocate timestamp GPU memory from the command allocator.
        // AllocateGpuScratchMem() always returns a valid GPU address, even if we fail to obtain memory from the
        // allocator.  In that scenario, the allocator returns a dummy chunk so we can always have a valid object
        // to access, and sets m_status to a failure code.
        m_timestampGpuVa = AllocateGpuScratchMem(sizeof(uint32), sizeof(uint32));
        result = m_status;
    }

    return result;
}

// =====================================================================================================================
// Helper method which returns all generated chunks to the parent allocator, removing our references to those chunks.
void GfxCmdBuffer::ReturnGeneratedCommandChunks(
    bool returnGpuMemory)
{
    if (returnGpuMemory)
    {
        // The client requested that we return all chunks, add any remaining retained chunks to the chunk list so they
        // can be returned to the allocator with the rest.
        while (m_retainedGeneratedChunkList.IsEmpty() == false)
        {
            CmdStreamChunk* pChunk = nullptr;
            m_retainedGeneratedChunkList.PopBack(&pChunk);
            m_generatedChunkList.PushBack(pChunk);
        }

        // Return all chunks containing GPU-generated commands to the allocator.
        if ((m_generatedChunkList.IsEmpty() == false) && (m_flags.autoMemoryReuse == true))
        {
            m_pCmdAllocator->ReuseChunks(EmbeddedDataAlloc, false, m_generatedChunkList.Begin());
        }
    }
    else
    {
        // Reset the chunks to be retained and add them to the retained list. We can only reset them here because
        // of the interface requirement that the client guarantee that no one is using this command stream anymore.
        for (auto iter = m_generatedChunkList.Begin(); iter.IsValid(); iter.Next())
        {
            iter.Get()->Reset();
            m_retainedGeneratedChunkList.PushBack(iter.Get());
        }
    }

    m_generatedChunkList.Clear();
}

// =====================================================================================================================
void GfxCmdBuffer::CmdCopyMemoryByGpuVa(
    gpusize                 srcGpuVirtAddr,
    gpusize                 dstGpuVirtAddr,
    uint32                  regionCount,
    const MemoryCopyRegion* pRegions)
{
    PAL_ASSERT(pRegions != nullptr);

    // We cannot know if the the P2P PCI BAR work around is required for the destination memory, so set an error
    // to make the client aware of the problem.
    if (m_device.Parent()->ChipProperties().p2pBltWaInfo.required)
    {
        SetCmdRecordingError(Result::ErrorIncompatibleDevice);
    }
    else
    {
        m_device.RsrcProcMgr().CopyMemoryCs(this,
                                            srcGpuVirtAddr,
                                            *m_device.Parent(),
                                            dstGpuVirtAddr,
                                            *m_device.Parent(),
                                            regionCount,
                                            pRegions,
                                            false,
                                            nullptr);
    }
}

// =====================================================================================================================
void GfxCmdBuffer::CmdCopyImage(
    const IImage&          srcImage,
    ImageLayout            srcImageLayout,
    const IImage&          dstImage,
    ImageLayout            dstImageLayout,
    uint32                 regionCount,
    const ImageCopyRegion* pRegions,
    const Rect*            pScissorRect,
    uint32                 flags)
{
    PAL_ASSERT(pRegions != nullptr);
    m_device.RsrcProcMgr().CmdCopyImage(this,
                                        static_cast<const Image&>(srcImage),
                                        srcImageLayout,
                                        static_cast<const Image&>(dstImage),
                                        dstImageLayout,
                                        regionCount,
                                        pRegions,
                                        pScissorRect,
                                        flags);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdCopyMemoryToImage(
    const IGpuMemory&            srcGpuMemory,
    const IImage&                dstImage,
    ImageLayout                  dstImageLayout,
    uint32                       regionCount,
    const MemoryImageCopyRegion* pRegions)
{
    PAL_ASSERT(pRegions != nullptr);
    m_device.RsrcProcMgr().CmdCopyMemoryToImage(this,
                                                static_cast<const GpuMemory&>(srcGpuMemory),
                                                static_cast<const Image&>(dstImage),
                                                dstImageLayout,
                                                regionCount,
                                                pRegions,
                                                false);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdCopyImageToMemory(
    const IImage&                srcImage,
    ImageLayout                  srcImageLayout,
    const IGpuMemory&            dstGpuMemory,
    uint32                       regionCount,
    const MemoryImageCopyRegion* pRegions)
{
    PAL_ASSERT(pRegions != nullptr);
    m_device.RsrcProcMgr().CmdCopyImageToMemory(this,
                                                static_cast<const Image&>(srcImage),
                                                srcImageLayout,
                                                static_cast<const GpuMemory&>(dstGpuMemory),
                                                regionCount,
                                                pRegions,
                                                false);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdCopyMemoryToTiledImage(
    const IGpuMemory&                 srcGpuMemory,
    const IImage&                     dstImage,
    ImageLayout                       dstImageLayout,
    uint32                            regionCount,
    const MemoryTiledImageCopyRegion* pRegions)
{
    PAL_ASSERT(pRegions != nullptr);

    AutoBuffer<MemoryImageCopyRegion, 8, Platform> copyRegions(regionCount, m_device.GetPlatform());

    if (copyRegions.Capacity() < regionCount)
    {
        NotifyAllocFailure();
    }
    else
    {
        const ImageMemoryLayout& imgMemLayout = static_cast<const Image&>(dstImage).GetMemoryLayout();
        const Extent3d tileSize = { imgMemLayout.prtTileWidth, imgMemLayout.prtTileHeight, imgMemLayout.prtTileDepth };

        for (uint32 i = 0; i < regionCount; ++i)
        {
            copyRegions[i].imageSubres         = pRegions[i].imageSubres;
            copyRegions[i].imageOffset.x       = pRegions[i].imageOffset.x * static_cast<int32>(tileSize.width);
            copyRegions[i].imageOffset.y       = pRegions[i].imageOffset.y * static_cast<int32>(tileSize.height);
            copyRegions[i].imageOffset.z       = pRegions[i].imageOffset.z * static_cast<int32>(tileSize.depth);
            copyRegions[i].imageExtent.width   = pRegions[i].imageExtent.width * tileSize.width;
            copyRegions[i].imageExtent.height  = pRegions[i].imageExtent.height * tileSize.height;
            copyRegions[i].imageExtent.depth   = pRegions[i].imageExtent.depth * tileSize.depth;
            copyRegions[i].numSlices           = pRegions[i].numSlices;
            copyRegions[i].gpuMemoryOffset     = pRegions[i].gpuMemoryOffset;
            copyRegions[i].gpuMemoryRowPitch   = pRegions[i].gpuMemoryRowPitch;
            copyRegions[i].gpuMemoryDepthPitch = pRegions[i].gpuMemoryDepthPitch;
            copyRegions[i].swizzledFormat      = {};
        }

        m_device.RsrcProcMgr().CmdCopyMemoryToImage(this,
                                                    static_cast<const GpuMemory&>(srcGpuMemory),
                                                    static_cast<const Image&>(dstImage),
                                                    dstImageLayout,
                                                    regionCount,
                                                    &copyRegions[0],
                                                    true);
    }
}

// =====================================================================================================================
void GfxCmdBuffer::CmdCopyTiledImageToMemory(
    const IImage&                     srcImage,
    ImageLayout                       srcImageLayout,
    const IGpuMemory&                 dstGpuMemory,
    uint32                            regionCount,
    const MemoryTiledImageCopyRegion* pRegions)
{
    PAL_ASSERT(pRegions != nullptr);

    AutoBuffer<MemoryImageCopyRegion, 8, Platform> copyRegions(regionCount, m_device.GetPlatform());

    if (copyRegions.Capacity() < regionCount)
    {
        NotifyAllocFailure();
    }
    else
    {
        const ImageMemoryLayout& imgMemLayout = static_cast<const Image&>(srcImage).GetMemoryLayout();
        const Extent3d tileSize = { imgMemLayout.prtTileWidth, imgMemLayout.prtTileHeight, imgMemLayout.prtTileDepth };

        for (uint32 i = 0; i < regionCount; ++i)
        {
            copyRegions[i].imageSubres         = pRegions[i].imageSubres;
            copyRegions[i].imageOffset.x       = pRegions[i].imageOffset.x * static_cast<int32>(tileSize.width);
            copyRegions[i].imageOffset.y       = pRegions[i].imageOffset.y * static_cast<int32>(tileSize.height);
            copyRegions[i].imageOffset.z       = pRegions[i].imageOffset.z * static_cast<int32>(tileSize.depth);
            copyRegions[i].imageExtent.width   = pRegions[i].imageExtent.width * tileSize.width;
            copyRegions[i].imageExtent.height  = pRegions[i].imageExtent.height * tileSize.height;
            copyRegions[i].imageExtent.depth   = pRegions[i].imageExtent.depth * tileSize.depth;
            copyRegions[i].numSlices           = pRegions[i].numSlices;
            copyRegions[i].gpuMemoryOffset     = pRegions[i].gpuMemoryOffset;
            copyRegions[i].gpuMemoryRowPitch   = pRegions[i].gpuMemoryRowPitch;
            copyRegions[i].gpuMemoryDepthPitch = pRegions[i].gpuMemoryDepthPitch;
            copyRegions[i].swizzledFormat      = {};
        }

        m_device.RsrcProcMgr().CmdCopyImageToMemory(this,
                                                    static_cast<const Image&>(srcImage),
                                                    srcImageLayout,
                                                    static_cast<const GpuMemory&>(dstGpuMemory),
                                                    regionCount,
                                                    &copyRegions[0],
                                                    true);
    }
}

// =====================================================================================================================
void GfxCmdBuffer::CmdCopyTypedBuffer(
    const IGpuMemory&            srcGpuMemory,
    const IGpuMemory&            dstGpuMemory,
    uint32                       regionCount,
    const TypedBufferCopyRegion* pRegions)
{
    PAL_ASSERT(pRegions != nullptr);
    m_device.RsrcProcMgr().CmdCopyTypedBuffer(this,
                                              static_cast<const GpuMemory&>(srcGpuMemory),
                                              static_cast<const GpuMemory&>(dstGpuMemory),
                                              regionCount,
                                              pRegions);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdScaledCopyImage(
    const ScaledCopyInfo& copyInfo)
{
    PAL_ASSERT(copyInfo.pRegions != nullptr);
    m_device.RsrcProcMgr().CmdScaledCopyImage(this, copyInfo);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdGenerateMipmaps(
    const GenMipmapsInfo& genInfo)
{
    m_device.RsrcProcMgr().CmdGenerateMipmaps(this, genInfo);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdColorSpaceConversionCopy(
    const IImage&                     srcImage,
    ImageLayout                       srcImageLayout,
    const IImage&                     dstImage,
    ImageLayout                       dstImageLayout,
    uint32                            regionCount,
    const ColorSpaceConversionRegion* pRegions,
    TexFilter                         filter,
    const ColorSpaceConversionTable&  cscTable)
{
    PAL_ASSERT(pRegions != nullptr);
    m_device.RsrcProcMgr().CmdColorSpaceConversionCopy(this,
                                                       static_cast<const Image&>(srcImage),
                                                       srcImageLayout,
                                                       static_cast<const Image&>(dstImage),
                                                       dstImageLayout,
                                                       regionCount,
                                                       pRegions,
                                                       filter,
                                                       cscTable);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdPostProcessFrame(
    const CmdPostProcessFrameInfo& postProcessInfo,
    bool*                          pAddedGpuWork)
{
    bool addedGpuWork = false;

    if (postProcessInfo.flags.srcIsTypedBuffer == 0)
    {
        const auto& image          = static_cast<const Image&>(*postProcessInfo.pSrcImage);
        const auto& presentedImage =
            image;

        // If developer mode is enabled, we need to apply the developer overlay.
        if (m_device.GetPlatform()->ShowDevDriverOverlay())
        {
            m_device.Parent()->ApplyDevOverlay(presentedImage, this);
            addedGpuWork = true;
        }

        if (image.GetGfxImage()->HasDisplayDccData())
        {
            // The surface must be fully expanded if another component may access it via PFPA,
            // or KMD nofify UMD to expand DCC.
            // Presentable surface has dcc and displayDcc, but turbo sync surface hasn't dcc,
            // before present, need decompress dcc when turbo sync enables.
            if (postProcessInfo.fullScreenFrameMetadataControlFlags.primaryHandle ||
                postProcessInfo.fullScreenFrameMetadataControlFlags.expandDcc     ||
                postProcessInfo.fullScreenFrameMetadataControlFlags.timerNodeSubmission)
            {
                BarrierInfo barrier = {};

                BarrierTransition transition = {};
                transition.srcCacheMask                    = CoherShader;
                transition.dstCacheMask                    = CoherShader;

                transition.imageInfo.pImage                = &image;
                transition.imageInfo.oldLayout.usages      = LayoutPresentWindowed | LayoutPresentFullscreen;
                transition.imageInfo.oldLayout.engines     = (GetEngineType() == EngineTypeUniversal) ?
                                                             LayoutUniversalEngine : LayoutComputeEngine;
                transition.imageInfo.newLayout.usages      = LayoutShaderRead | LayoutUncompressed;
                transition.imageInfo.newLayout.engines     = transition.imageInfo.oldLayout.engines;
                transition.imageInfo.subresRange.numPlanes = 1;
                transition.imageInfo.subresRange.numMips   = 1;
                transition.imageInfo.subresRange.numSlices = 1;

                barrier.pTransitions = &transition;
                barrier.transitionCount = 1;

                barrier.waitPoint = HwPipePreCs;

                HwPipePoint pipePoints = HwPipeTop;
                barrier.pPipePoints = &pipePoints;
                barrier.pipePointWaitCount = 1;

                CmdBarrier(barrier);

                // if Dcc is decompressed, needn't do retile, put displayDCC memory
                // itself back into a "fully decompressed" state.
                m_device.RsrcProcMgr().CmdDisplayDccFixUp(this, image);
                addedGpuWork = true;
            }
            else if (m_device.CoreSettings().displayDccSkipRetileBlt == false)
            {
                m_device.RsrcProcMgr().CmdGfxDccToDisplayDcc(this, image);
                addedGpuWork = true;
            }
        }
    }

    if (addedGpuWork && (pAddedGpuWork != nullptr))
    {
        *pAddedGpuWork = true;
    }

#if PAL_BUILD_RDF
    m_device.GetPlatform()->UpdateFrameTraceController(this);
#endif
}

// =====================================================================================================================
// For BLT presents, this function on GfxCmdBuffer will perform whatever operations are necessary to copy the image data
// from the source image to the destination image.
void GfxCmdBuffer::CmdPresentBlt(
    const IImage&   srcImage,
    const IImage&   dstImage,
    const Offset3d& dstOffset)
{
    const auto& srcImageInfo  = srcImage.GetImageCreateInfo();

    ImageScaledCopyRegion region = {};
    region.srcExtent.width  = srcImageInfo.extent.width;
    region.srcExtent.height = srcImageInfo.extent.height;
    region.srcExtent.depth  = 1;
    region.dstExtent        = region.srcExtent;
    region.dstOffset        = dstOffset;
    region.numSlices        = 1;

    const ImageLayout srcLayout =
    {
        LayoutPresentWindowed,
        (GetEngineType() == EngineTypeUniversal) ? LayoutUniversalEngine : LayoutComputeEngine
    };

    const ImageLayout dstLayout =
    {
        LayoutCopyDst,
        (GetEngineType() == EngineTypeUniversal) ? LayoutUniversalEngine : LayoutComputeEngine
    };

    ScaledCopyInfo      copyInfo         = {};
    constexpr TexFilter DefaultTexFilter = {};

    copyInfo.pSrcImage              = &srcImage;
    copyInfo.srcImageLayout         = srcLayout;
    copyInfo.pDstImage              = &dstImage;
    copyInfo.dstImageLayout         = dstLayout;
    copyInfo.regionCount            = 1;
    copyInfo.pRegions               = &region;
    copyInfo.filter                 = DefaultTexFilter;
    copyInfo.rotation               = ImageRotation::Ccw0;
    copyInfo.pColorKey              = nullptr;
    copyInfo.flags.srcColorKey      = false;
    copyInfo.flags.dstAsSrgb        = false;

    m_device.RsrcProcMgr().CmdScaledCopyImage(this, copyInfo);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdFillMemory(
    const IGpuMemory& dstGpuMemory,
    gpusize           dstOffset,
    gpusize           fillSize,
    uint32            data)
{
    m_device.RsrcProcMgr().CmdFillMemory(this,
                                         (IsComputeStateSaved() == false),
                                         static_cast<const GpuMemory&>(dstGpuMemory),
                                         dstOffset,
                                         fillSize,
                                         data);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdClearColorBuffer(
    const IGpuMemory& gpuMemory,
    const ClearColor& color,
    SwizzledFormat    bufferFormat,
    uint32            bufferOffset,
    uint32            bufferExtent,
    uint32            rangeCount,
    const Range*      pRanges)
{
    m_device.RsrcProcMgr().CmdClearColorBuffer(this,
                                               gpuMemory,
                                               color,
                                               bufferFormat,
                                               bufferOffset,
                                               bufferExtent,
                                               rangeCount,
                                               pRanges);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdClearBoundColorTargets(
    uint32                          colorTargetCount,
    const BoundColorTarget*         pBoundColorTargets,
    uint32                          regionCount,
    const ClearBoundTargetRegion*   pClearRegions)
{
    m_device.RsrcProcMgr().CmdClearBoundColorTargets(this,
                                                     colorTargetCount,
                                                     pBoundColorTargets,
                                                     regionCount,
                                                     pClearRegions);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdClearColorImage(
    const IImage&      image,
    ImageLayout        imageLayout,
    const ClearColor&  color,
    uint32             rangeCount,
    const SubresRange* pRanges,
    uint32             boxCount,
    const Box*         pBoxes,
    uint32             flags)
{
    PAL_ASSERT(pRanges != nullptr);

    uint32 splitRangeCount;
    bool splitMemAllocated          = false;
    const SubresRange* pSplitRanges = nullptr;
    Result result = m_device.Parent()->SplitSubresRanges(rangeCount,
                                                         pRanges,
                                                         &splitRangeCount,
                                                         &pSplitRanges,
                                                         &splitMemAllocated);

    if (result == Result::ErrorOutOfMemory)
    {
        NotifyAllocFailure();
    }
    else if (result == Result::Success)
    {
        m_device.RsrcProcMgr().CmdClearColorImage(this,
                                                  static_cast<const Image&>(image),
                                                  imageLayout,
                                                  color,
                                                  splitRangeCount,
                                                  pSplitRanges,
                                                  boxCount,
                                                  pBoxes,
                                                  flags);
    }
    else
    {
        PAL_ASSERT_ALWAYS();
    }

    // Delete memory allocated for splitting the BarrierTransitions if necessary.
    if (splitMemAllocated)
    {
        PAL_SAFE_DELETE_ARRAY(pSplitRanges, m_device.GetPlatform());
    }
}

// =====================================================================================================================
void GfxCmdBuffer::CmdClearBoundDepthStencilTargets(
    float                         depth,
    uint8                         stencil,
    uint8                         stencilWriteMask,
    uint32                        samples,
    uint32                        fragments,
    DepthStencilSelectFlags       flag,
    uint32                        regionCount,
    const ClearBoundTargetRegion* pClearRegions)
{
    m_device.RsrcProcMgr().CmdClearBoundDepthStencilTargets(this,
                                                            depth,
                                                            stencil,
                                                            stencilWriteMask,
                                                            samples,
                                                            fragments,
                                                            flag,
                                                            regionCount,
                                                            pClearRegions);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdClearDepthStencil(
    const IImage&      image,
    ImageLayout        depthLayout,
    ImageLayout        stencilLayout,
    float              depth,
    uint8              stencil,
    uint8              stencilWriteMask,
    uint32             rangeCount,
    const SubresRange* pRanges,
    uint32             rectCount,
    const Rect*        pRects,
    uint32             flags)
{
    PAL_ASSERT(pRanges != nullptr);

    uint32 splitRangeCount;
    bool splitMemAllocated          = false;
    const SubresRange* pSplitRanges = nullptr;
    Result result = m_device.Parent()->SplitSubresRanges(rangeCount,
                                                         pRanges,
                                                         &splitRangeCount,
                                                         &pSplitRanges,
                                                         &splitMemAllocated);

    if (result == Result::ErrorOutOfMemory)
    {
        NotifyAllocFailure();
    }
    else if (result == Result::Success)
    {
        m_device.RsrcProcMgr().CmdClearDepthStencil(this,
                                                    static_cast<const Image&>(image),
                                                    depthLayout,
                                                    stencilLayout,
                                                    depth,
                                                    stencil,
                                                    stencilWriteMask,
                                                    splitRangeCount,
                                                    pSplitRanges,
                                                    rectCount,
                                                    pRects,
                                                    flags);
    }
    else
    {
        PAL_ASSERT_ALWAYS();
    }

    // Delete memory allocated for splitting the BarrierTransitions if necessary.
    if (splitMemAllocated)
    {
        PAL_SAFE_DELETE_ARRAY(pSplitRanges, m_device.GetPlatform());
    }
}

// =====================================================================================================================
void GfxCmdBuffer::CmdClearBufferView(
    const IGpuMemory& gpuMemory,
    const ClearColor& color,
    const void*       pBufferViewSrd,
    uint32            rangeCount,
    const Range*      pRanges)
{
    PAL_ASSERT(pBufferViewSrd != nullptr);
    m_device.RsrcProcMgr().CmdClearBufferView(this, gpuMemory, color, pBufferViewSrd, rangeCount, pRanges);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdClearImageView(
    const IImage&     image,
    ImageLayout       imageLayout,
    const ClearColor& color,
    const void*       pImageViewSrd,
    uint32            rectCount,
    const Rect*       pRects)
{
     PAL_ASSERT(pImageViewSrd != nullptr);
     m_device.RsrcProcMgr().CmdClearImageView(this,
                                              static_cast<const Image&>(image),
                                              imageLayout,
                                              color,
                                              pImageViewSrd,
                                              rectCount,
                                              pRects);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdResolveImage(
    const IImage&             srcImage,
    ImageLayout               srcImageLayout,
    const IImage&             dstImage,
    ImageLayout               dstImageLayout,
    ResolveMode               resolveMode,
    uint32                    regionCount,
    const ImageResolveRegion* pRegions,
    uint32                    flags)
{
    PAL_ASSERT(pRegions != nullptr);
    m_device.RsrcProcMgr().CmdResolveImage(this,
                                           static_cast<const Image&>(srcImage),
                                           srcImageLayout,
                                           static_cast<const Image&>(dstImage),
                                           dstImageLayout,
                                           resolveMode,
                                           regionCount,
                                           pRegions,
                                           flags);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdResolvePrtPlusImage(
    const IImage&                    srcImage,
    ImageLayout                      srcImageLayout,
    const IImage&                    dstImage,
    ImageLayout                      dstImageLayout,
    PrtPlusResolveType               resolveType,
    uint32                           regionCount,
    const PrtPlusImageResolveRegion* pRegions)
{
    PAL_ASSERT(pRegions != nullptr);

    const auto&  dstCreateInfo = dstImage.GetImageCreateInfo();
    const auto&  srcCreateInfo = srcImage.GetImageCreateInfo();

    // Either the source or destination image has to be a PRT map image
    if (((resolveType == PrtPlusResolveType::Decode) && (srcCreateInfo.prtPlus.mapType != PrtMapType::None)) ||
        ((resolveType == PrtPlusResolveType::Encode) && (dstCreateInfo.prtPlus.mapType != PrtMapType::None)))
    {
        m_device.RsrcProcMgr().CmdResolvePrtPlusImage(this,
                                                      static_cast<const Image&>(srcImage),
                                                      srcImageLayout,
                                                      static_cast<const Image&>(dstImage),
                                                      dstImageLayout,
                                                      resolveType,
                                                      regionCount,
                                                      pRegions);
    }
}

// =====================================================================================================================
// Copies the requested portion of the currently bound compute state to m_computeRestoreState. All active queries will
// be disabled. This cannot be called again until CmdRestoreComputeState is called.
void GfxCmdBuffer::CmdSaveComputeState(
    uint32 stateFlags)
{
    PAL_ASSERT(IsComputeStateSaved() == false);
    m_computeStateFlags = stateFlags;

    if (TestAnyFlagSet(stateFlags, ComputeStatePipelineAndUserData))
    {
        // It should be impossible to already have this allocated because we null it out on restore.
        PAL_ASSERT(m_computeRestoreState.pKernelArguments == nullptr);

        // Copy over the bound pipeline and all non-indirect user-data state.
        m_computeRestoreState = m_computeState;

        // In HSA mode we must also duplicate the dynamically allocated current kernel argument buffer.
        if (m_computeState.hsaAbiMode)
        {
            // It's impossible to be in HSA mode without a pipeline.
            PAL_ASSERT(m_computeState.pipelineState.pPipeline != nullptr);

            const auto&  pipeline = *static_cast<const ComputePipeline*>(m_computeState.pipelineState.pPipeline);
            const uint32 size     = pipeline.HsaMetadata().KernargSegmentSize();

            m_computeRestoreState.pKernelArguments =
                static_cast<uint8*>(PAL_MALLOC(size, m_device.GetPlatform(), AllocInternal));

            if (m_computeRestoreState.pKernelArguments == nullptr)
            {
                // Allocation failure, mark buffer as faulty.
                NotifyAllocFailure();
            }
            else
            {
                memcpy(m_computeRestoreState.pKernelArguments, m_computeState.pKernelArguments, size);
            }
        }
    }

    if (TestAnyFlagSet(stateFlags, ComputeStateBorderColorPalette))
    {
        // Copy over the bound border color palette.
        m_computeRestoreState.pipelineState.pBorderColorPalette = m_computeState.pipelineState.pBorderColorPalette;
    }

    if (m_pCurrentExperiment != nullptr)
    {
        // Inform the performance experiment that we're starting some internal operations.
        m_pCurrentExperiment->BeginInternalOps(GetCmdStreamByEngine(GetPerfExperimentEngine()));
    }
}

// =====================================================================================================================
// Restores the requested portion of the last saved compute state in m_computeRestoreState, rebinding all objects as
// necessary. All previously disabled queries will be reactivated.
void GfxCmdBuffer::CmdRestoreComputeState(
    uint32 stateFlags)
{
    PAL_ASSERT(TestAllFlagsSet(m_computeStateFlags, stateFlags));
    m_computeStateFlags = 0;

    // Vulkan does allow blits in nested command buffers, but they do not support inheriting user-data values from
    // the caller. Therefore, simply "setting" the restored-state's user-data is sufficient, just like it is in a
    // root command buffer. (If Vulkan decides to support user-data inheritance in a later API version, we'll need
    // to revisit this!)

    SetComputeState(m_computeRestoreState, stateFlags);

    // We may have allocated this if we saved while in HSA mode. It makes things simpler if we just free it now.
    PAL_SAFE_FREE(m_computeRestoreState.pKernelArguments, m_device.GetPlatform());

    if (m_pCurrentExperiment != nullptr)
    {
        // Inform the performance experiment that we've finished some internal operations.
        m_pCurrentExperiment->EndInternalOps(GetCmdStreamByEngine(GetPerfExperimentEngine()));
    }
}

// =====================================================================================================================
// Set all specified state on this command buffer.
void GfxCmdBuffer::SetComputeState(
    const ComputeState& newComputeState,
    uint32              stateFlags)
{
    if (TestAnyFlagSet(stateFlags, ComputeStatePipelineAndUserData))
    {
        if (newComputeState.pipelineState.pPipeline != m_computeState.pipelineState.pPipeline)
        {
            PipelineBindParams bindParams = {};
            bindParams.pipelineBindPoint  = PipelineBindPoint::Compute;
            bindParams.pPipeline          = newComputeState.pipelineState.pPipeline;
            bindParams.cs                 = newComputeState.dynamicCsInfo;
            bindParams.apiPsoHash         = newComputeState.pipelineState.apiPsoHash;

            CmdBindPipeline(bindParams);
        }

        // We're only supposed to save/restore kernel args in HSA mode and user-data in PAL mode.
        if (m_computeState.hsaAbiMode)
        {
            // It's impossible to be in HSA mode without a pipeline.
            PAL_ASSERT(m_computeState.pipelineState.pPipeline != nullptr);

            // By now the current pKernelArguments must have the same size as the saved original. We must memcpy it
            // because this function is used in places where we can't just assume ownership of the saved copy's buffer.
            const auto&  pipeline = *static_cast<const ComputePipeline*>(m_computeState.pipelineState.pPipeline);
            const uint32 size     = pipeline.HsaMetadata().KernargSegmentSize();

            memcpy(m_computeState.pKernelArguments, newComputeState.pKernelArguments, size);
        }
        else
        {
            CmdSetUserData(PipelineBindPoint::Compute,
                           0,
                           m_device.Parent()->ChipProperties().gfxip.maxUserDataEntries,
                           &newComputeState.csUserDataEntries.entries[0]);
        }
    }

    if (TestAnyFlagSet(stateFlags, ComputeStateBorderColorPalette) &&
        (newComputeState.pipelineState.pBorderColorPalette != m_computeState.pipelineState.pBorderColorPalette))
    {
        CmdBindBorderColorPalette(PipelineBindPoint::Compute, newComputeState.pipelineState.pBorderColorPalette);
    }
}

// =====================================================================================================================
// CmdSetUserData callback which updates the tracked user-data entries for the compute state.
void PAL_STDCALL GfxCmdBuffer::CmdSetUserDataCs(
    ICmdBuffer*   pCmdBuffer,
    uint32        firstEntry,
    uint32        entryCount,
    const uint32* pEntryValues)
{
    PAL_ASSERT((pCmdBuffer != nullptr) && (entryCount != 0) && (pEntryValues != nullptr));

    auto*const pThis    = static_cast<GfxCmdBuffer*>(pCmdBuffer);
    auto*const pEntries = &pThis->m_computeState.csUserDataEntries;

    // It's illegal to bind user-data when in HSA ABI mode.
    PAL_ASSERT(pThis->m_computeState.hsaAbiMode == false);

    // NOTE: Compute operations are expected to be far rarer than graphics ones, so at the moment it is not expected
    // that filtering-out redundant compute user-data updates is worthwhile.
    for (uint32 e = firstEntry; e < (firstEntry + entryCount); ++e)
    {
        WideBitfieldSetBit(pEntries->touched, e);
        WideBitfieldSetBit(pEntries->dirty,   e);
    }
    memcpy(&pEntries->entries[firstEntry], pEntryValues, entryCount * sizeof(uint32));
}

// =====================================================================================================================
CmdBufferEngineSupport GfxCmdBuffer::GetPerfExperimentEngine() const
{
    return (TestAnyFlagSet(m_engineSupport, CmdBufferEngineSupport::Graphics)
            ? CmdBufferEngineSupport::Graphics
            : CmdBufferEngineSupport::Compute);
}

// =====================================================================================================================
void GfxCmdBuffer::CmdCopyImageToPackedPixelImage(
        const IImage&          srcImage,
        const IImage&          dstImage,
        uint32                 regionCount,
        const ImageCopyRegion* pRegions,
        Pal::PackedPixelType   packPixelType)
{
    PAL_ASSERT(pRegions != nullptr);
    m_device.RsrcProcMgr().CopyImageToPackedPixelImage(
        this,
        static_cast<const Image&>(srcImage),
        static_cast<const Image&>(dstImage),
        regionCount,
        pRegions,
        packPixelType);
}

// =====================================================================================================================
uint32 GfxCmdBuffer::GetUsedSize(
    CmdAllocType type
) const
{
    uint32 sizeInBytes = CmdBuffer::GetUsedSize(type);

    if (type == CommandDataAlloc)
    {
        uint32 cmdDataSizeInDwords = 0;
        for (auto iter = m_generatedChunkList.Begin(); iter.IsValid(); iter.Next())
        {
            cmdDataSizeInDwords += iter.Get()->DwordsAllocated();
        }

        sizeInBytes += cmdDataSizeInDwords * sizeof(uint32);
    }

    return sizeInBytes;
}

} // Pal

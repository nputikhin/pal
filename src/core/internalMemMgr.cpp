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

#include "g_coreSettings.h"
#include "core/device.h"
#include "core/internalMemMgr.h"
#include "core/platform.h"
#include "palBuddyAllocatorImpl.h"
#include "palGpuMemoryBindable.h"
#include "palListImpl.h"
#include "palSysMemory.h"
#include <stdio.h>

using namespace Util;

namespace Pal
{

static constexpr gpusize DefaultPoolAllocationSize    = 1ull << 22; // 4 megabytes
static constexpr gpusize DefaultPoolAllocationMinSize = 1ull << 16; // 64 kilobytes
static constexpr gpusize PoolMinSuballocationSize     = 1ull << 4;  // 16 bytes

// =====================================================================================================================
// Determines whether a base allocation matches the requested parameters
static bool IsMatchingPool(
    const GpuMemoryPool&    pool,
    bool                    readOnly,
    GpuMemoryFlags          memFlags,
    size_t                  heapCount,
    const GpuHeap           (&heaps)[GpuHeapCount],
    VaRange                 vaRange,
    MType                   mtype)
{
    bool matches = true;

    if ((pool.memFlags.u64All == memFlags.u64All) &&
        (pool.heapCount       == heapCount)       &&
        (pool.readOnly        == readOnly)        &&
        (pool.vaRange         == vaRange)         &&
        (pool.mtype           == mtype))
    {
        for (uint32 h = 0; h < heapCount; ++h)
        {
            if (pool.heaps[h] != heaps[h])
            {
                matches = false;
                break;
            }
        }
    }
    else
    {
        matches = false;
    }

    return matches;
}

// =====================================================================================================================
// Initializes a set of GPU memory flags based on the values contained in the GPU memory create info and internal
// create info structures. This is an incomplete conversion for the flags, only sufficient for the Buddy Allocator's
// comparison to see if memory objects are compatible.
static GpuMemoryFlags ConvertGpuMemoryFlags(
    const GpuMemoryCreateInfo&         createInfo,
    const GpuMemoryInternalCreateInfo& internalInfo)
{
    GpuMemoryFlags flags{};

    flags.isShareable    = createInfo.flags.shareable;
    flags.isFlippable    = createInfo.flags.flippable;
    flags.interprocess   = createInfo.flags.interprocess;
    flags.isStereo       = createInfo.flags.stereo;
    flags.autoPriority   = createInfo.flags.autoPriority;
    flags.tmzProtected   = createInfo.flags.tmzProtected;
    flags.pageDirectory  = internalInfo.flags.pageDirectory;
    flags.pageTableBlock = internalInfo.flags.pageTableBlock;
    flags.udmaBuffer     = internalInfo.flags.udmaBuffer;
    flags.xdmaBuffer     = internalInfo.flags.xdmaBuffer;
    flags.alwaysResident = internalInfo.flags.alwaysResident;
    flags.buddyAllocated = 1;

    return flags;
}

// =====================================================================================================================
// Filter invisible heap. For some objects as pipeline, invisible heap will be appended in memory requirement.
// Internal use as RPM pipeline/overlay pipeline ought to filter the invisible heap before use.
static void FilterInvisibleHeap(
    GpuMemoryRequirements* pMemReq)
{
    uint32 origHeapCount = pMemReq->heapCount;

    pMemReq->heapCount = 0;

    for (uint32 i = 0; i< origHeapCount; ++i)
    {
        if (pMemReq->heaps[i] != GpuHeapInvisible)
        {
            pMemReq->heaps[pMemReq->heapCount++] = pMemReq->heaps[i];
        }
    }
}

// =====================================================================================================================
InternalMemMgr::InternalMemMgr(
    Device* pDevice)
    :
    m_pDevice(pDevice),
    m_poolList(pDevice->GetPlatform()),
    m_references(pDevice->GetPlatform()),
    m_referenceWatermark(0)
{
}

// =====================================================================================================================
// Explicitly frees all GPU memory allocations.
void InternalMemMgr::FreeAllocations()
{
    // Delete the GPU memory objects using the references list
    while (m_references.NumElements() != 0)
    {
        auto it = GetRefListIter();

        PAL_ASSERT((it.Get() != nullptr) && (it.Get()->pGpuMemory != nullptr));

        // Free the GPU memory object
        it.Get()->pGpuMemory->DestroyInternal();
        it.Get()->pGpuMemory = nullptr;

        // Remove the list entry
        m_references.Erase(&it);
    }

    while (m_poolList.NumElements() != 0)
    {
        auto it = m_poolList.Begin();

        PAL_ASSERT((it.Get() != nullptr) && (it.Get()->pBuddyAllocator != nullptr));

        // Destroy the sub-allocator
        PAL_DELETE(it.Get()->pBuddyAllocator, m_pDevice->GetPlatform());

        // Remove the list entry
        m_poolList.Erase(&it);
    }
}

// =====================================================================================================================
// Allocates GPU memory for internal use, ensures thread safety by acquiring the allocator lock.
Result InternalMemMgr::AllocateGpuMem(
    const GpuMemoryCreateInfo&          createInfo,
    const GpuMemoryInternalCreateInfo&  internalInfo,
    bool                                readOnly,
    GpuMemory**                         ppGpuMemory,
    gpusize*                            pOffset)
{
    Util::MutexAuto allocatorLock(&m_allocatorLock); // Ensure thread-safety using the lock

    return AllocateGpuMemNoAllocLock(createInfo, internalInfo, readOnly, ppGpuMemory, pOffset);
}

// =====================================================================================================================
// Assuming the caller is already holding the allocator lock, this function will not use the lock.
//
// Allocates GPU memory for internal use. Depending on the type of memory object requested, the memory may be
// sub-allocated from an existing allocation, or it might not.
//
// The sub-allocation scheme is skipped if pOffset is null.
//
// Any new allocations are added to the memory manager's list of internal memory references.
Result InternalMemMgr::AllocateGpuMemNoAllocLock(
    const GpuMemoryCreateInfo&          createInfo,
    const GpuMemoryInternalCreateInfo&  internalInfo,
    bool                                readOnly,
    GpuMemory**                         ppGpuMemory,
    gpusize*                            pOffset)
{
    Result result = Result::Success;

    // It doesn't make sense to suballocate virtual memory; this class assumes it only allocates real memory objects.
    PAL_ASSERT(createInfo.flags.virtualAlloc == 0);

    // By convention, the only allocations that are allowed to skip the sub-allocation scheme are are UDMA buffers, page
    // directories, and page-table blocks, and PageFaultDebugSrds. We may relax this if there is good reason to skip sub-allocation for other
    // kinds of allocations.
    PAL_ASSERT(((internalInfo.flags.udmaBuffer        == 0) &&
                (internalInfo.flags.pageDirectory     == 0) &&
                (internalInfo.flags.pageTableBlock    == 0) &&
                (internalInfo.flags.isCmdAllocator    == 0) &&
                (internalInfo.flags.pageFaultDebugSrd == 0)) == (pOffset != nullptr));

    GpuMemoryCreateInfo localCreateInfo = createInfo;

    // TMZ allocations can only be allocated from heaps that support TMZ. The caller must provide at least one TMZ heap.
    if (localCreateInfo.flags.tmzProtected)
    {
        localCreateInfo.heapCount = 0;
        for (uint32 i = 0; i < createInfo.heapCount; i++)
        {
            if (m_pDevice->HeapProperties(createInfo.heaps[i]).flags.supportsTmz)
            {
                localCreateInfo.heaps[localCreateInfo.heapCount++] = createInfo.heaps[i];
            }
        }
        if (localCreateInfo.heapCount == 0)
        {
            result = Result::ErrorInvalidValue;
        }
    }

    // If the requested allocation is small enough, try to find an appropriate pool and sub-allocate from it.
    if ((result                    == Result::Success)             &&
        (pOffset                   != nullptr)                     &&
        (localCreateInfo.size      <= DefaultPoolAllocationSize / 2) &&
        (localCreateInfo.alignment <= DefaultPoolAllocationSize / 2))
    {
        // Calculate GPU memory flags based on the creation information
        const GpuMemoryFlags requestedMemFlags = ConvertGpuMemoryFlags(localCreateInfo, internalInfo);

        result = Result::ErrorOutOfMemory;

        // CurrentPoolSize will be double in first use.
        gpusize currentPoolSize = DefaultPoolAllocationMinSize / 2;

        // Try to find a base allocation of the appropriate type that has sufficient enough space
        for (auto it = m_poolList.Begin(); it.Get() != nullptr; it.Next())
        {
            GpuMemoryPool* pPool = it.Get();

            if (IsMatchingPool(*pPool,
                               readOnly,
                               requestedMemFlags,
                               localCreateInfo.heapCount,
                               localCreateInfo.heaps,
                               localCreateInfo.vaRange,
                               internalInfo.mtype))
            {
                const GpuMemoryDesc& desc = pPool->pGpuMemory->Desc();

                // Now pool size is not fixed value. Make sure sub allocation size is matching with this pool.
                if ((localCreateInfo.size      <= desc.size / 2) &&
                    (localCreateInfo.alignment <= desc.size / 2))
                {
                    // If the base allocation matches the search criteria then try to allocate from it
                    result = pPool->pBuddyAllocator->Allocate(localCreateInfo.size,
                                                              localCreateInfo.alignment,
                                                              pOffset);

                    if (result == Result::Success)
                    {
                        // If we found a free block, fill in the memory object pointer from the base allocation and
                        // stop searching
                        *ppGpuMemory = pPool->pGpuMemory;
                        if (internalInfo.pPagingFence != nullptr)
                        {
                            *internalInfo.pPagingFence = pPool->pagingFenceVal;
                        }
                        break;
                    }
                }

                // Loop to find the max size we have in the matched pools.
                currentPoolSize = Util::Max(currentPoolSize, desc.size);
            }
        }

        if (result != Result::Success)
        {
            // None of the existing base allocations had a free block large enough for us so we need to create
            // a new base allocation

            // Fix-up the GPU memory create info structures to suit the base allocation's needs
            GpuMemoryInternalCreateInfo localInternalInfo  = internalInfo;

            const gpusize localCreateInfoAlignment = localCreateInfo.alignment;
            const gpusize localCreateInfoSize      = localCreateInfo.size;

            // Enlarge next pool allocation as double current max pool size
            gpusize nextPoolAllocationSize = currentPoolSize * 2;

            // Check if need to enlarge the pool size base on creation allocate size and alignment
            nextPoolAllocationSize = Util::Max(Util::Pow2Pad(localCreateInfoSize * 2), nextPoolAllocationSize);
            nextPoolAllocationSize = Util::Max(Util::Pow2Pad(localCreateInfoAlignment * 2), nextPoolAllocationSize);

            // Clamp to maximum size
            nextPoolAllocationSize = Util::Min(DefaultPoolAllocationSize, nextPoolAllocationSize);

            localCreateInfo.size                   = nextPoolAllocationSize;
            localCreateInfo.alignment              = nextPoolAllocationSize / 2;
            localInternalInfo.flags.buddyAllocated = 1;

            GpuMemory* pGpuMemory = nullptr;

            // Issue the base memory allocation
            result = AllocateBaseGpuMem(localCreateInfo, localInternalInfo, readOnly, &pGpuMemory);
            localCreateInfo.size = localCreateInfoSize;
            localCreateInfo.alignment = localCreateInfoAlignment;

            if (result == Result::Success)
            {
                // We need to add the newly allocated base allocation to the list
                GpuMemoryPool newPool = {};

                newPool.pGpuMemory = pGpuMemory;
                newPool.readOnly   = readOnly;
                newPool.memFlags   = requestedMemFlags;
                newPool.heapCount  = localCreateInfo.heapCount;
                newPool.vaRange    = localCreateInfo.vaRange;
                newPool.mtype      = internalInfo.mtype;
                if (internalInfo.pPagingFence != nullptr)
                {
                    newPool.pagingFenceVal = *internalInfo.pPagingFence;
                }

                for (uint32 h = 0; h < localCreateInfo.heapCount; ++h)
                {
                    newPool.heaps[h] = localCreateInfo.heaps[h];
                }

                // Create and initialize the buddy allocator
                newPool.pBuddyAllocator = PAL_NEW(BuddyAllocator<Platform>, m_pDevice->GetPlatform(), AllocInternal)
                                          (m_pDevice->GetPlatform(),
                                           nextPoolAllocationSize,
                                           PoolMinSuballocationSize);

                if (newPool.pBuddyAllocator != nullptr)
                {
                    // Try to initialize the buddy allocator
                    result = newPool.pBuddyAllocator->Init();

                    gpusize localOffset = 0;

                    if (result == Result::Success)
                    {
                        // ... and then sub-allocate from it
                        // NOTE: The sub-allocation should never fail here since we just optained a fresh base
                        // allocation, the only possible case for failure is a low system memory situation
                        result = newPool.pBuddyAllocator->Allocate(localCreateInfo.size,
                                                                   localCreateInfo.alignment,
                                                                   &localOffset);
                    }

                    // If we successfully sub-allocated from the new buddy allocator, then attempt to add the new pool
                    // to the list
                    if (result == Result::Success)
                    {
                        result = m_poolList.PushFront(newPool);
                    }

                    // Finally, if absolutely everything succeeded, return values to caller
                    if (result == Result::Success)
                    {
                        *ppGpuMemory = pGpuMemory;
                        *pOffset     = localOffset;
                    }
                }
                else
                {
                    result = Result::ErrorOutOfMemory;
                }

                // Undo any allocations if something went wrong
                if (result != Result::Success)
                {
                    // Delete the buddy allocator if it exists.
                    PAL_DELETE(newPool.pBuddyAllocator, m_pDevice->GetPlatform());

                    // If there was a failure then release the base allocation
                    FreeBaseGpuMem(pGpuMemory);
                }
            }
        }
    }
    else if (result == Result::Success)
    {
        if (pOffset != nullptr)
        {
            // Since we're not sub-allocating, the new memory object will always have a zero offset.
            *pOffset = 0;

            // General-purpose calls to AllocateGpuMem shouldn't trigger a base mem allocation. If this alert tiggers
            // it's a sign that we might need to tune our buddy allocator.
            PAL_ALERT_ALWAYS_MSG("Memory could not be suballocated, consider tweaking pool/allocator settings.");
        }

        // Issue the base memory allocation.
        result = AllocateBaseGpuMem(localCreateInfo, internalInfo, readOnly, ppGpuMemory);
    }

    return result;
}

// =====================================================================================================================
// Allocates a base GPU memory object allocation.
Result InternalMemMgr::AllocateBaseGpuMem(
    const GpuMemoryCreateInfo&          createInfo,
    const GpuMemoryInternalCreateInfo&  internalInfo,
    bool                                readOnly,
    GpuMemory**                         ppGpuMemory)
{
    // All memory allocated by the internal mem mgr should be always resident.
    PAL_ASSERT(internalInfo.flags.alwaysResident);

    Result result = m_pDevice->CreateInternalGpuMemory(createInfo, internalInfo, ppGpuMemory);

    if (IsErrorResult(result) == false)
    {
        // We need to add the newly created allocation to the reference list
        RWLockAuto<RWLock::ReadWrite> referenceLock(&m_referenceLock);

        GpuMemoryInfo memInfo = {};
        memInfo.pGpuMemory  = *ppGpuMemory;
        memInfo.readOnly    = readOnly;
        result = m_references.PushBack(memInfo);

        if (result == Result::Success)
        {
            // If we succeeded then increment the reference list watermark and return with success
            m_referenceWatermark++;
        }
        else
        {
            // If there was a failure then release the GPU memory object
            (*ppGpuMemory)->DestroyInternal();
            *ppGpuMemory = nullptr;
        }
    }

    return result;
}

// =====================================================================================================================
// Queries the provided GPU-memory-bindable object for its memory requirements, and allocates GPU memory to satisfy
// those requirements. Finally, the memory is bound to the provided object if allocation is successful.
Result InternalMemMgr::AllocateAndBindGpuMem(
    IGpuMemoryBindable* pBindable,
    bool                readOnly)
{
    // Get the memory requirements of the GPU-memory-bindable object
    GpuMemoryRequirements memReqs = {};
    pBindable->GetGpuMemoryRequirements(&memReqs);

    // Fill in the GPU memory object creation info based on the memory requirements
    GpuMemoryCreateInfo createInfo = {};
    createInfo.size         = memReqs.size;
    createInfo.alignment    = memReqs.alignment;
    createInfo.priority     = GpuMemPriority::Normal;
    createInfo.heapCount    = memReqs.heapCount;
    for (uint32 i = 0; i < memReqs.heapCount; ++i)
    {
        createInfo.heaps[i] = memReqs.heaps[i];
    }

    GpuMemoryInternalCreateInfo internalInfo = {};
    internalInfo.flags.alwaysResident = 1;

    GpuMemory*  pGpuMemory  = nullptr;
    gpusize     offset      = 0;

    // Issue the memory allocation
    Result result = AllocateGpuMem(createInfo, internalInfo, readOnly, &pGpuMemory, &offset);

    if (result == Result::Success)
    {
        // If the memory allocation succeeded then try to bind the memory to the object
        result = pBindable->BindGpuMemory(pGpuMemory, offset);

        if (result != Result::Success)
        {
            // If binding the memory failed then free the allocated memory
            FreeGpuMem(pGpuMemory, offset);
        }
    }

    return result;
}

// =====================================================================================================================
// Frees GPU memory which was previously allocated for internal use.
Result InternalMemMgr::FreeGpuMem(
    GpuMemory*  pGpuMemory,
    gpusize     offset)
{
    PAL_ASSERT(pGpuMemory != nullptr);

    Result result = Result::ErrorInvalidValue;

    if (pGpuMemory->WasBuddyAllocated())
    {
        MutexAuto allocatorLock(&m_allocatorLock); // Ensure thread-safety using the lock

        // Try to find the allocation in the pool list
        for (auto it = m_poolList.Begin(); it.Get() != nullptr; it.Next())
        {
            GpuMemoryPool* pPool = it.Get();

            PAL_ASSERT((pPool->pGpuMemory != nullptr) && (pPool->pBuddyAllocator != nullptr));

            if (pPool->pGpuMemory == pGpuMemory)
            {
                // If found then use the buddy allocator to release the block
                pPool->pBuddyAllocator->Free(offset);

                result = Result::Success;
                break;
            }
        }

        // If we didn't find the allocation in the pool list then something went wrong with the allocation scheme
        PAL_ASSERT(result == Result::Success);
    }
    else
    {
        PAL_ASSERT(offset == 0); // We don't expect allocation offsets for anything which wasn't buddy allocated

        // Free a base allocation
        result = FreeBaseGpuMem(pGpuMemory);
    }

    return result;
}

// =====================================================================================================================
// Frees a base GPU memory object allocation that was created by the internal memory manager.
Result InternalMemMgr::FreeBaseGpuMem(
    GpuMemory*  pGpuMemory)
{
    Result result = Result::ErrorInvalidValue;

    // This scope is just to minimize the duration of holding the references lock.
    {
        RWLockAuto<RWLock::ReadWrite> referenceLock(&m_referenceLock);

        // Try to find the allocation in the reference list
        for (auto it = GetRefListIter(); it.Get() != nullptr; it.Next())
        {
            GpuMemoryInfo* pMemInfo = it.Get();

            PAL_ASSERT(pMemInfo->pGpuMemory != nullptr);

            if (pGpuMemory == pMemInfo->pGpuMemory)
            {
                // Also remove the reference list item and increment the watermark
                m_references.Erase(&it);
                m_referenceWatermark++;

                result = Result::Success;
                break;
            }
        }
    }

    // Release the GPU memory object.
    // This must be done after releasing the references lock because some platforms may take a different lock to do
    // internal bookkeeping when releasing GPU memory.
    pGpuMemory->DestroyInternal();

    // If we didn't find the allocation in the reference list then something went wrong with the allocation scheme
    PAL_ASSERT(result == Result::Success);

    return result;
}

// =====================================================================================================================
// Get number of elements of m_reference
uint32 InternalMemMgr::GetReferencesCount()
{
    RWLockAuto<RWLock::ReadOnly> referenceLock(&m_referenceLock);
    return static_cast<uint32>(m_references.NumElements());
}

} // Pal

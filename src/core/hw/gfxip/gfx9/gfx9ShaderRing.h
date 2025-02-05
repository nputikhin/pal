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

#pragma once

#include "core/hw/gfxip/gfx9/gfx9Chip.h"
#include "core/gpuMemory.h"
#include "palLiterals.h"

namespace Pal
{
namespace Gfx9
{

class Device;

// =====================================================================================================================
// Base class for all shader-ring types. Provides defaults for computing the ring video memory size as well as handling
// the memory allocation.
class ShaderRing
{
public:
    virtual ~ShaderRing();

    Result Validate(size_t itemSize, ShaderRingMemory* pDeferredMem);

    bool IsMemoryValid() const { return m_ringMem.IsBound(); }

    gpusize GpuVirtAddr() const { return m_ringMem.GpuVirtAddr(); }

    gpusize MemorySizeBytes() const { return m_allocSize; }
    gpusize MemorySizeDwords() const { return (m_allocSize / sizeof(uint32)); }

    // Returns the shader Ring's maximum supported Item Size. The units and meaning of this value depends on which ring
    // you are referring to.
    size_t ItemSizeMax() const { return m_itemSizeMax; }

protected:
    ShaderRing(Device* pDevice, BufferSrd* pSrdTable, bool isTmz, ShaderRingType type);

    virtual Result AllocateVideoMemory(gpusize memorySizeBytes, ShaderRingMemory* pDeferredMem);

    virtual gpusize ComputeAllocationSize() const;

    // Informs the Shader Ring to update its SRD's.
    virtual void UpdateSrds() const = 0;

    Device*const         m_pDevice;
    BufferSrd*const      m_pSrdTable;   // Pointer to the parent ring-set's SRD table
    BoundGpuMemory       m_ringMem;     // Shader-ring video memory allocation
    const bool           m_tmzEnabled;  // Shader-ring video memory allocated on protected memory
    gpusize              m_allocSize;   // Current "real" video memory size (in bytes)
    size_t               m_numMaxWaves; // Max. number of waves allowed to execute in parallel
    size_t               m_itemSizeMax; // Highest item size this Ring has needed so far
    const ShaderRingType m_ringType;
    const GfxIpLevel     m_gfxLevel;

private:
    PAL_DISALLOW_DEFAULT_CTOR(ShaderRing);
    PAL_DISALLOW_COPY_AND_ASSIGN(ShaderRing);
};

// =====================================================================================================================
// Implements shader-ring functionality specific for shader scratch memory.
class ScratchRing final : public ShaderRing
{
public:
    ScratchRing(Device* pDevice, BufferSrd* pSrdTable, Pm4ShaderType shaderType, bool isTmz);
    virtual ~ScratchRing() {}

    size_t CalculateWaves() const;
    size_t CalculateWaveSize() const;

protected:
    virtual gpusize ComputeAllocationSize() const override;
    virtual void UpdateSrds() const override;

private:
    size_t AdjustScratchWaveSize(size_t scratchWaveSize) const;

    const Pm4ShaderType m_shaderType;
    uint32              m_numTotalCus;
    size_t              m_scratchWaveSizeGranularityShift;
    size_t              m_scratchWaveSizeGranularity;

    PAL_DISALLOW_DEFAULT_CTOR(ScratchRing);
    PAL_DISALLOW_COPY_AND_ASSIGN(ScratchRing);
};

// =====================================================================================================================
// Implements shader-ring functionality specific to the GS/VS shader ring required to support normal (i.e. off-chip) GS.
class GsVsRing final : public ShaderRing
{
public:
    GsVsRing(Device* pDevice, BufferSrd* pSrdTable, bool isTmz);
    virtual ~GsVsRing() {}

protected:
    virtual void UpdateSrds() const override;

private:
    // Number of SRD's written to by the GS/VS ring.
    static constexpr size_t WriteSrds = 4;
    // Total number of SRD's referenced by the GS/VS ring: four for write, one for read.
    static constexpr size_t TotalSrds = (WriteSrds + 1);

    // Fixed number of records for the GS/VS write SRD's:
    static constexpr uint32 NumRecordsWrite = 64;

    PAL_DISALLOW_DEFAULT_CTOR(GsVsRing);
    PAL_DISALLOW_COPY_AND_ASSIGN(GsVsRing);
};

// =====================================================================================================================
// Implements shader-ring functionality specific to the Tess-Factor Buffer required to support tessellation.
class TessFactorBuffer final : public ShaderRing
{
public:
    TessFactorBuffer(Device* pDevice, BufferSrd* pSrdTable, bool isTmz);
    virtual ~TessFactorBuffer() {}

    uint32 TfRingSize() const;

protected:
    virtual gpusize ComputeAllocationSize() const override;
    virtual void UpdateSrds() const override;

private:
    PAL_DISALLOW_DEFAULT_CTOR(TessFactorBuffer);
    PAL_DISALLOW_COPY_AND_ASSIGN(TessFactorBuffer);
};

// =====================================================================================================================
// Implements shader-ring functionality specific to the Offchip LDS Buffers required for offchip tessellation.
class OffchipLdsBuffer final : public ShaderRing
{
public:
    OffchipLdsBuffer(Device* pDevice, BufferSrd* pSrdTable, bool isTmz);
    virtual ~OffchipLdsBuffer() {}

    uint32 OffchipBuffering() const;

protected:
    virtual gpusize ComputeAllocationSize() const override;
    virtual void UpdateSrds() const override;

private:
    PAL_DISALLOW_DEFAULT_CTOR(OffchipLdsBuffer);
    PAL_DISALLOW_COPY_AND_ASSIGN(OffchipLdsBuffer);
};

// =====================================================================================================================
// Implements shader-ring functionality specific to the sample position buffer required for AMDIL samplepos.
class SamplePosBuffer final : public ShaderRing
{
public:
    SamplePosBuffer(Device* pDevice, BufferSrd* pSrdTable, bool isTmz);
    virtual ~SamplePosBuffer() {}

    void UploadSamplePatternPalette(const SamplePatternPalette& samplePatternPalette);

protected:
    virtual gpusize ComputeAllocationSize() const override;
    virtual void UpdateSrds() const override;

private:
    PAL_DISALLOW_DEFAULT_CTOR(SamplePosBuffer);
    PAL_DISALLOW_COPY_AND_ASSIGN(SamplePosBuffer);
};

// =====================================================================================================================
// Implements shader-ring functionality specific for shader scratch memory.
class MeshScratchRing final : public ShaderRing
{
public:
    MeshScratchRing(Device* pDevice, BufferSrd* pSrdTable, bool isTmz);
    virtual ~MeshScratchRing() {}

protected:
    virtual gpusize ComputeAllocationSize() const override;
    virtual void UpdateSrds() const override;

private:
    uint32 m_maxThreadgroupsPerChip;

    PAL_DISALLOW_DEFAULT_CTOR(MeshScratchRing);
    PAL_DISALLOW_COPY_AND_ASSIGN(MeshScratchRing);
};

// =====================================================================================================================
// Implement shader-ring functionality specific to the PayloadData buffer required for Task -> GFX shader functionality.
class PayloadDataRing final : public ShaderRing
{
public:
    PayloadDataRing(Device* pDevice, BufferSrd* pSrdTable, bool isTmz);
    virtual ~PayloadDataRing() {}

protected:
    virtual gpusize ComputeAllocationSize() const override
        { return m_maxNumEntries * PayloadDataEntrySize; }
    virtual void UpdateSrds() const override;

private:
    static constexpr uint32 PayloadDataEntrySize = 16 * Util::OneKibibyte;  // This is the expected maximum size by the APIs.
    const uint32 m_maxNumEntries;

    PAL_DISALLOW_DEFAULT_CTOR(PayloadDataRing);
    PAL_DISALLOW_COPY_AND_ASSIGN(PayloadDataRing);
};

// =====================================================================================================================
// Implement shader-ring functionality specific to the IndirectDraw arguments necessary to launch GFX shaders after a
// Task shader dispatch.
class DrawDataRing final : public ShaderRing
{
public:
    DrawDataRing(Device* pDevice, BufferSrd* pSrdTable, bool isTmz);
    virtual ~DrawDataRing() {}

    uint32 GetNumEntries() const { return m_maxNumEntries; }
    void Initialize();

protected:
    virtual gpusize ComputeAllocationSize() const override
        { return m_maxNumEntries * DrawDataEntrySize; }
    virtual void UpdateSrds() const override;

    struct DrawDataRingLayout
    {
        uint32 xDim;
        uint32 yDim;
        uint32 zDim;
        union
        {
            struct
            {
                uint32 drawReady : 1;
                uint32 packetEnd : 1;
                uint32 reserved0 : 30;
            };

            uint32 u32All;
        };
    };

private:
    // This is the expected size of this data by the CP.
    static constexpr uint32 DrawDataEntrySize = sizeof(DrawDataRingLayout);
    const uint32 m_maxNumEntries;

    PAL_DISALLOW_DEFAULT_CTOR(DrawDataRing);
    PAL_DISALLOW_COPY_AND_ASSIGN(DrawDataRing);
};

// =====================================================================================================================
// Implements shader-ring functionality specific to the TASKMESH control buffer.
class TaskMeshControlRing final : public ShaderRing
{
public:
    TaskMeshControlRing(Device* pDevice, BufferSrd* pSrdTable, bool isTmz);
    virtual ~TaskMeshControlRing() {}
    void InitializeControlBuffer(gpusize drawRingAddr, uint32 numEntries);

protected:
#pragma pack(push, 1)
    struct ControlBufferLayout
    {
        uint64 writePtr;
        uint64 readPtr;
        uint64 deallocPtr;
        uint32 numEntries;
        uint64 drawRingBaseAddr;
    };
#pragma pack(pop)
    static_assert(sizeof(ControlBufferLayout) == sizeof(uint32) * 9,
                  "Control buffer is a different size than expected!");

    virtual gpusize ComputeAllocationSize() const override { return sizeof(ControlBufferLayout); }
    virtual void UpdateSrds() const override {};

private:
    PAL_DISALLOW_DEFAULT_CTOR(TaskMeshControlRing);
    PAL_DISALLOW_COPY_AND_ASSIGN(TaskMeshControlRing);
};

} // Gfx9
} // Pal

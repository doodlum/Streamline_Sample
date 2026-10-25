//----------------------------------------------------------------------------------
// File:        NSSWrapper.h
//
// Arm Neural Super Sampling (NSS) as an alternative upscaler to DLSS, using the
// Neural Graphics SDK "universal" branch. NSS is Vulkan only; on GPUs without
// VK_ARM_data_graph the SDK runs the network with the vendor's own ML acceleration
// (cooperative matrices / int8 dot products), so it runs on NVIDIA, AMD and Intel.
//----------------------------------------------------------------------------------
#pragma once

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string>

struct VkDeviceCreateInfo;

class NSSWrapper
{
public:
    static NSSWrapper& Get();

    // Called from donut's deviceCreateInfoCallback (Vulkan only): enables the device features and
    // extensions the NSS ML compute path can use, as far as the physical device supports them.
    void PrepareVulkanDeviceCreate(void* instance, void* physicalDevice, void* getInstanceProcAddr, VkDeviceCreateInfo& createInfo);

    // NSS needs a Vulkan device; this is false on D3D.
    bool IsAvailable(nvrhi::IDevice* device) const;

    struct Settings
    {
        donut::math::int2 renderSize;
        donut::math::int2 displaySize;
        int               quality       = 1;  // 0 = Quality, 1 = Balanced, 2 = Performance (network size)
        bool              depthInverted = true;
        bool              depthInfinite = false;  // infinite far plane (donut's perspProjD3DStyleReverse)
    };

    struct Inputs
    {
        nvrhi::ITexture*    color         = nullptr;  // HDR scene colour, render resolution
        nvrhi::ITexture*    depth         = nullptr;  // depth (any depth format), render resolution
        nvrhi::ITexture*    motionVectors = nullptr;  // RG16F pixel-space motion vectors, render resolution
        nvrhi::ITexture*    output        = nullptr;  // RGBA16F UAV, display resolution
        donut::math::float2 jitterOffset  = {0.f, 0.f};  // pixels
        float               cameraNear    = 0.1f;
        float               cameraFar     = 200.f;
        float               cameraFovY    = 1.f;     // radians
        float               frameTimeMs   = 16.6f;
        bool                reset         = false;
    };

    // Compute shader (NSSDepthCopy.hlsl) that copies scene depth to R32F; NSS cannot view depth-stencil formats.
    void SetDepthCopyShader(nvrhi::IShader* shader) { m_DepthCopyShader = shader; }

    // Creates (or re-creates on a settings change) the NSS context.
    bool Configure(nvrhi::IDevice* device, const Settings& settings);
    void Evaluate(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, const Inputs& inputs);
    void Shutdown();
    // Destroys the context and every GPU object; call before the device goes away.
    void Release();

    // e.g. "NSS Balanced: NVIDIA GeForce RTX 4080: coopmat int8 16x16x32" (from the SDK log)
    const std::string& GetStatus() const { return m_Status; }
    nvrhi::ITexture*   GetDepthCopy() const { return m_DepthR32; }

private:
    NSSWrapper() = default;
    void EnsureDepthCopy(nvrhi::IDevice* device, donut::math::int2 size);
    // NSS_REPLAY_DIR=<capture dir>: substitutes the inputs recorded by a capture run with NSS_RECORD_DIR set (params.txt)
    // and CAPTURE_DUMP_UPSCALED + CAPTURE_DUMP_MV (input/mv/depth_NNNN.bin), so shader changes can be checked bit for bit.
    bool ReplayInputs(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, const Inputs& in, Inputs& out);

    void*       m_Context = nullptr;
    Settings    m_Settings{};
    bool        m_HasSettings = false;
    uint32_t    m_MlFeatures  = 0;
    std::string m_Status      = "NSS: not initialised";

    nvrhi::TextureHandle         m_DepthR32;
    nvrhi::TextureHandle         m_ReplayColor;
    nvrhi::TextureHandle         m_ReplayMotion;
    uint32_t                     m_EvalIndex = 0;
    nvrhi::ShaderHandle          m_DepthCopyShader;
    nvrhi::BindingLayoutHandle   m_DepthCopyLayout;
    nvrhi::ComputePipelineHandle m_DepthCopyPipeline;
};

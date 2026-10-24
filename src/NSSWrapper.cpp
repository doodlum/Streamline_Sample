//----------------------------------------------------------------------------------
// File:        NSSWrapper.cpp
//
// Arm Neural Super Sampling integration (Vulkan). See NSSWrapper.h.
//----------------------------------------------------------------------------------
#include "NSSWrapper.h"

#include <donut/core/log.h>

#ifdef _WIN32
#include <Windows.h>
#endif

#include <vulkan/vulkan.h>

#include <ffx_api/ffx_api.hpp>
#include <ffx_api/ffx_nss.hpp>
#include <ffx_api/vk/ffx_api_vk.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// From the SDK's Vulkan backend (ffx_vk.h); declared here to avoid pulling the backend headers into the sample.
uint32_t ffxQueryMlDeviceFeaturesVK(VkInstance instance, VkPhysicalDevice physicalDevice, PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                                                 const char** extensionNames, uint32_t* extensionCount);

namespace
{
    std::string g_LastPathMessage;

    void OnNssMessage(uint32_t type, const char* message)
    {
        if (!message)
            return;
        std::string text(message);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (text.empty())
            return;
        if (text.find("compute path") != std::string::npos)
            g_LastPathMessage = text;
        if (type == FFX_API_MESSAGE_TYPE_ERROR)
            donut::log::error("[NSS] %s", text.c_str());
        else
            donut::log::info("[NSS] %s", text.c_str());
    }

    PFN_vkGetInstanceProcAddr GetInstanceProcAddr()
    {
#ifdef _WIN32
        // With Streamline the app's Vulkan loader is sl.interposer.dll.
        for (const char* module : {"sl.interposer.dll", "vulkan-1.dll"})
        {
            if (HMODULE handle = GetModuleHandleA(module))
            {
                if (auto fn = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(handle, "vkGetInstanceProcAddr")))
                    return fn;
            }
        }
#endif
        return nullptr;
    }

    VkFormat ToVkFormat(nvrhi::Format format)
    {
        switch (format)
        {
        case nvrhi::Format::RGBA16_FLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case nvrhi::Format::RG16_FLOAT:
            return VK_FORMAT_R16G16_SFLOAT;
        case nvrhi::Format::R32_FLOAT:
            return VK_FORMAT_R32_SFLOAT;
        case nvrhi::Format::RGBA32_FLOAT:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case nvrhi::Format::R11G11B10_FLOAT:
            return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }

    FfxApiResource WrapTexture(nvrhi::ITexture* texture, uint32_t state)
    {
        const nvrhi::TextureDesc& desc = texture->getDesc();
        VkImageCreateInfo         info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType                 = VK_IMAGE_TYPE_2D;
        info.format                    = ToVkFormat(desc.format);
        info.extent                    = {desc.width, desc.height, 1};
        info.mipLevels                 = desc.mipLevels;
        info.arrayLayers               = desc.arraySize;
        info.usage                     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (desc.isUAV)
            info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (desc.isRenderTarget)
            info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        VkImage image = reinterpret_cast<VkImage>(texture->getNativeObject(nvrhi::ObjectTypes::VK_Image).pointer);
        const FfxApiResourceDescription resourceDesc = ffxApiGetImageResourceDescriptionVK(image, info, 0);
        return ffxApiGetResourceVK(reinterpret_cast<void*>(image), resourceDesc, state);
    }

    template <typename T>
    T* FindInChain(const void* pNext, VkStructureType type)
    {
        for (auto* it = reinterpret_cast<const VkBaseInStructure*>(pNext); it; it = it->pNext)
        {
            if (it->sType == type)
                return reinterpret_cast<T*>(const_cast<VkBaseInStructure*>(it));
        }
        return nullptr;
    }
}  // namespace

NSSWrapper& NSSWrapper::Get()
{
    static NSSWrapper instance;
    return instance;
}

void NSSWrapper::PrepareVulkanDeviceCreate(void* instancePtr, void* physicalDevicePtr, void* getInstanceProcAddrPtr, VkDeviceCreateInfo& createInfo)
{
    VkInstance                instance            = reinterpret_cast<VkInstance>(instancePtr);
    VkPhysicalDevice          physicalDevice      = reinterpret_cast<VkPhysicalDevice>(physicalDevicePtr);
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(getInstanceProcAddrPtr);

    const char* required[8]   = {};
    uint32_t    requiredCount = 8;
    const uint32_t supported  = ffxQueryMlDeviceFeaturesVK(instance, physicalDevice, getInstanceProcAddr, required, &requiredCount);

    auto* v11 = FindInChain<VkPhysicalDeviceVulkan11Features>(createInfo.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    auto* v12 = FindInChain<VkPhysicalDeviceVulkan12Features>(createInfo.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    auto* v13 = FindInChain<VkPhysicalDeviceVulkan13Features>(createInfo.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    if (!v11 || !v12 || !v13)
    {
        donut::log::warning("NSS: Vulkan 1.3 feature structures not found, NSS will be unavailable");
        return;
    }

    uint32_t enabled = 0;
    auto     enable  = [&](uint32_t flag, VkBool32& field) {
        if (supported & flag)
        {
            field = VK_TRUE;
            enabled |= flag;
        }
    };
    enable(FFX_API_VK_ML_FEATURE_SHADER_INT8, v12->shaderInt8);
    enable(FFX_API_VK_ML_FEATURE_STORAGE_BUFFER_8BIT, v12->storageBuffer8BitAccess);
    enable(FFX_API_VK_ML_FEATURE_SHADER_FLOAT16, v12->shaderFloat16);
    enable(FFX_API_VK_ML_FEATURE_STORAGE_BUFFER_16BIT, v11->storageBuffer16BitAccess);
    enable(FFX_API_VK_ML_FEATURE_INTEGER_DOT_PRODUCT, v13->shaderIntegerDotProduct);
    if (supported & FFX_API_VK_ML_FEATURE_VULKAN_MEMORY_MODEL)
    {
        v12->vulkanMemoryModel            = VK_TRUE;
        v12->vulkanMemoryModelDeviceScope = VK_TRUE;
        enabled |= FFX_API_VK_ML_FEATURE_VULKAN_MEMORY_MODEL;
    }
    if (supported & FFX_API_VK_ML_FEATURE_SUBGROUP_SIZE_CONTROL)
    {
        v13->subgroupSizeControl  = VK_TRUE;
        v13->computeFullSubgroups = VK_TRUE;
        enabled |= FFX_API_VK_ML_FEATURE_SUBGROUP_SIZE_CONTROL;
    }
    v13->synchronization2 = VK_TRUE;  // required by the SDK

    // Extra feature structures and extensions for the vendor matrix units.
    static VkPhysicalDeviceCooperativeMatrixFeaturesKHR coopMatrix = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    static VkPhysicalDeviceShaderBfloat16FeaturesKHR    bfloat16   = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_BFLOAT16_FEATURES_KHR};
    static std::vector<const char*>                     extensions;
    extensions.assign(createInfo.ppEnabledExtensionNames, createInfo.ppEnabledExtensionNames + createInfo.enabledExtensionCount);
    auto addExtension = [&](const char* name) {
        for (const char* e : extensions)
            if (strcmp(e, name) == 0)
                return;
        extensions.push_back(name);
    };
    for (uint32_t i = 0; i < requiredCount; ++i)
        addExtension(required[i]);
    createInfo.enabledExtensionCount   = uint32_t(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    if ((supported & FFX_API_VK_ML_FEATURE_COOPERATIVE_MATRIX) &&
        !FindInChain<VkPhysicalDeviceCooperativeMatrixFeaturesKHR>(createInfo.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR))
    {
        coopMatrix.cooperativeMatrix = VK_TRUE;
        coopMatrix.pNext             = const_cast<void*>(createInfo.pNext);
        createInfo.pNext             = &coopMatrix;
        enabled |= FFX_API_VK_ML_FEATURE_COOPERATIVE_MATRIX;
    }
    if ((supported & FFX_API_VK_ML_FEATURE_SHADER_BFLOAT16) &&
        !FindInChain<VkPhysicalDeviceShaderBfloat16FeaturesKHR>(createInfo.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_BFLOAT16_FEATURES_KHR))
    {
        bfloat16.shaderBFloat16Type              = VK_TRUE;
        bfloat16.shaderBFloat16CooperativeMatrix = VK_TRUE;
        bfloat16.pNext                           = const_cast<void*>(createInfo.pNext);
        createInfo.pNext                         = &bfloat16;
        enabled |= FFX_API_VK_ML_FEATURE_SHADER_BFLOAT16;
    }

    m_MlFeatures = enabled;
    donut::log::info("NSS: enabled ML device features 0x%03x (supported 0x%03x)", enabled, supported);
}

bool NSSWrapper::IsAvailable(nvrhi::IDevice* device) const
{
    return device && device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN && (m_MlFeatures & FFX_API_VK_ML_FEATURE_SHADER_INT8);
}

bool NSSWrapper::Configure(nvrhi::IDevice* device, const Settings& settings)
{
    if (!IsAvailable(device))
        return false;
    if (m_Context && m_HasSettings && all(settings.renderSize == m_Settings.renderSize) && all(settings.displaySize == m_Settings.displaySize) &&
        settings.quality == m_Settings.quality && settings.depthInverted == m_Settings.depthInverted && settings.depthInfinite == m_Settings.depthInfinite)
        return true;

    device->waitForIdle();
    Shutdown();

    PFN_vkGetInstanceProcAddr gipa = GetInstanceProcAddr();
    if (!gipa)
    {
        m_Status = "NSS: vkGetInstanceProcAddr not found";
        return false;
    }

    ffx::CreateBackendVKDesc backendDesc{};
    backendDesc.vkInstance            = reinterpret_cast<VkInstance>(device->getNativeObject(nvrhi::ObjectTypes::VK_Instance).pointer);
    backendDesc.vkPhysicalDevice      = reinterpret_cast<VkPhysicalDevice>(device->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice).pointer);
    backendDesc.vkDevice              = reinterpret_cast<VkDevice>(device->getNativeObject(nvrhi::ObjectTypes::VK_Device).pointer);
    backendDesc.vkGetInstanceProcAddr = gipa;
    backendDesc.vkDeviceProcAddr      = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(backendDesc.vkInstance, "vkGetDeviceProcAddr"));

    ffx::CreateBackendVKMlFeaturesDesc mlDesc{};
    mlDesc.enabledFeatures = m_MlFeatures;

    ffx::CreateContextDescNss nssDesc{};
    nssDesc.maxRenderSize  = {uint32_t(settings.renderSize.x), uint32_t(settings.renderSize.y)};
    nssDesc.maxUpscaleSize = {uint32_t(settings.displaySize.x), uint32_t(settings.displaySize.y)};
    nssDesc.flags          = FFX_API_NSS_CONTEXT_FLAG_QUANTIZED | FFX_API_NSS_CONTEXT_FLAG_HIGH_DYNAMIC_RANGE | FFX_API_NSS_CONTEXT_FLAG_MANAGE_HISTORY |
                    (settings.depthInverted ? FFX_API_NSS_CONTEXT_FLAG_DEPTH_INVERTED : 0u) |
                    (settings.depthInfinite ? FFX_API_NSS_CONTEXT_FLAG_DEPTH_INFINITE : 0u);
    // NSS_DEPTH_FLAGS="inverted,infinite" (0/1 each) overrides for experiments.
    if (const char* env = getenv("NSS_DEPTH_FLAGS"))
    {
        int inv = 0, inf = 0;
        sscanf(env, "%d,%d", &inv, &inf);
        nssDesc.flags &= ~(FFX_API_NSS_CONTEXT_FLAG_DEPTH_INVERTED | FFX_API_NSS_CONTEXT_FLAG_DEPTH_INFINITE);
        nssDesc.flags |= (inv ? FFX_API_NSS_CONTEXT_FLAG_DEPTH_INVERTED : 0u) | (inf ? FFX_API_NSS_CONTEXT_FLAG_DEPTH_INFINITE : 0u);
    }
    nssDesc.fpMessage   = OnNssMessage;
    nssDesc.qualityMode = static_cast<FfxApiNssShaderQualityMode>(settings.quality);

    g_LastPathMessage.clear();
    ffxContext     context = nullptr;
    ffx::ReturnCode result  = ffx::CreateContext(context, nullptr, nssDesc, backendDesc, mlDesc);
    if (result != ffx::ReturnCode::Ok)
    {
        m_Status = "NSS: context creation failed (" + std::to_string(uint32_t(result)) + ")";
        donut::log::error("%s", m_Status.c_str());
        return false;
    }

    static const char* kQualityNames[] = {"Quality", "Balanced", "Performance"};
    m_Context     = context;
    m_Settings    = settings;
    m_HasSettings = true;
    m_Status      = std::string("NSS ") + kQualityNames[settings.quality % 3] + " " + std::to_string(settings.renderSize.x) + "x" +
               std::to_string(settings.renderSize.y) + " -> " + std::to_string(settings.displaySize.x) + "x" + std::to_string(settings.displaySize.y);
    if (!g_LastPathMessage.empty())
        m_Status += "\n" + g_LastPathMessage;
    donut::log::info("%s", m_Status.c_str());
    return true;
}

void NSSWrapper::EnsureDepthCopy(nvrhi::IDevice* device, donut::math::int2 size)
{
    if (m_DepthR32 && int(m_DepthR32->getDesc().width) == size.x && int(m_DepthR32->getDesc().height) == size.y)
        return;

    nvrhi::TextureDesc desc;
    desc.width            = size.x;
    desc.height           = size.y;
    desc.format           = nvrhi::Format::R32_FLOAT;
    desc.isUAV            = true;
    desc.initialState     = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName        = "NSS_DepthR32";
    m_DepthR32            = device->createTexture(desc);

    if (!m_DepthCopyLayout)
    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.bindings   = {nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_UAV(0)};
        // The sample's shaders are compiled with t/b/u register shifts of 200/300/400 for Vulkan.
        layoutDesc.bindingOffsets.setShaderResourceOffset(200).setConstantBufferOffset(300).setUnorderedAccessViewOffset(400);
        m_DepthCopyLayout     = device->createBindingLayout(layoutDesc);

        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.CS            = m_DepthCopyShader;
        pipelineDesc.bindingLayouts = {m_DepthCopyLayout};
        m_DepthCopyPipeline        = device->createComputePipeline(pipelineDesc);
    }
}

void NSSWrapper::Evaluate(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, const Inputs& in)
{
    if (!m_Context || !m_DepthCopyShader)
        return;

    const nvrhi::TextureDesc& colorDesc = in.color->getDesc();
    const donut::math::int2   renderSize = m_Settings.renderSize;
    EnsureDepthCopy(device, renderSize);

    // Depth -> R32F
    {
        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {nvrhi::BindingSetItem::Texture_SRV(0, in.depth), nvrhi::BindingSetItem::Texture_UAV(0, m_DepthR32)};
        nvrhi::BindingSetHandle set = device->createBindingSet(setDesc, m_DepthCopyLayout);
        nvrhi::ComputeState     state;
        state.pipeline = m_DepthCopyPipeline;
        state.bindings = {set};
        commandList->setComputeState(state);
        commandList->dispatch((renderSize.x + 7) / 8, (renderSize.y + 7) / 8);
    }

    commandList->setTextureState(in.color, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(m_DepthR32, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(in.motionVectors, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(in.output, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    ffx::DispatchDescNss desc{};
    desc.commandList            = commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer;
    desc.color                  = WrapTexture(in.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.depth                  = WrapTexture(m_DepthR32, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.motionVectors          = WrapTexture(in.motionVectors, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.output                 = WrapTexture(in.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // NSS's jitter is the negative of donut's pixel offset on both axes (measured: static-scene PSNR vs DLSS
    // 38 dB with (-1,-1) against 29-32 dB for the other sign combinations). NSS_JITTER_SIGN="sx,sy" overrides.
    static float jitterSign[2] = {-1.f, -1.f};
    static bool  jitterSignRead = false;
    if (!jitterSignRead)
    {
        jitterSignRead = true;
        if (const char* env = getenv("NSS_JITTER_SIGN"))
            sscanf(env, "%f,%f", &jitterSign[0], &jitterSign[1]);
    }
    desc.jitterOffset = {jitterSign[0] * in.jitterOffset.x, jitterSign[1] * in.jitterOffset.y};
    desc.renderSize             = {uint32_t(renderSize.x), uint32_t(renderSize.y)};
    desc.upscaleSize            = {uint32_t(m_Settings.displaySize.x), uint32_t(m_Settings.displaySize.y)};
    desc.cameraNear             = in.cameraNear;
    desc.cameraFar              = in.cameraFar;
    desc.cameraFovAngleVertical = in.cameraFovY;
    desc.exposure               = 1.0f;
    // donut motion vectors are backward, in render-resolution pixels. NSS_MV_SCALE="sx,sy" overrides for experiments.
    static float mvScale[2] = {1.f, 1.f};
    static bool  mvScaleRead = false;
    if (!mvScaleRead)
    {
        mvScaleRead = true;
        if (const char* env = getenv("NSS_MV_SCALE"))
            sscanf(env, "%f,%f", &mvScale[0], &mvScale[1]);
    }
    desc.motionVectorScale = {mvScale[0], mvScale[1]};
    desc.frameTimeDelta         = in.frameTimeMs;
    desc.reset                  = in.reset;
    (void)colorDesc;

    ffxContext context = reinterpret_cast<ffxContext>(m_Context);
    if (ffx::Dispatch(context, desc) != ffx::ReturnCode::Ok)
        donut::log::error("NSS dispatch failed");

    // The SDK recorded raw Vulkan commands; make nvrhi re-emit its bindings.
    commandList->clearState();
}

void NSSWrapper::Shutdown()
{
    if (m_Context)
    {
        ffxContext context = reinterpret_cast<ffxContext>(m_Context);
        ffx::DestroyContext(context);
        m_Context = nullptr;
    }
    m_HasSettings = false;
}

void NSSWrapper::Release()
{
    Shutdown();
    m_DepthCopyPipeline = nullptr;
    m_DepthCopyLayout   = nullptr;
    m_DepthCopyShader   = nullptr;
    m_DepthR32          = nullptr;
}

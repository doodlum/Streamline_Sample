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
    m_EvalIndex   = 0;
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

    // Input replay for bit-exact shader comparisons (NSS_REPLAY_DIR, see ReplayInputs): replaces this frame's inputs with
    // ones recorded by an earlier capture run, so two builds can be fed identical frames.
    Inputs        replayed  = in;
    const bool    replaying = ReplayInputs(device, commandList, in, replayed);
    const Inputs& frameIn   = replaying ? replayed : in;

    // Depth -> R32F (a replayed frame uploads the recorded R32F depth instead)
    if (!replaying)
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

    commandList->setTextureState(frameIn.color, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(m_DepthR32, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(frameIn.motionVectors, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(in.output, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    ffx::DispatchDescNss desc{};
    desc.commandList            = commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer;
    desc.color                  = WrapTexture(frameIn.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.depth                  = WrapTexture(m_DepthR32, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.motionVectors          = WrapTexture(frameIn.motionVectors, FFX_API_RESOURCE_STATE_COMPUTE_READ);
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
    desc.jitterOffset = {jitterSign[0] * frameIn.jitterOffset.x, jitterSign[1] * frameIn.jitterOffset.y};
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
    desc.frameTimeDelta         = frameIn.frameTimeMs;
    desc.reset                  = frameIn.reset;
    (void)colorDesc;

    // NSS_RECORD_DIR: per-frame parameters for a later replay (the capture writes the textures, see StreamlineSample).
    if (const char* recordDir = getenv("NSS_RECORD_DIR"))
    {
        if (FILE* f = fopen((std::string(recordDir) + "/params.txt").c_str(), m_EvalIndex == 0 ? "w" : "a"))
        {
            fprintf(f, "%u %.9g %.9g %d %.9g\n", m_EvalIndex, in.jitterOffset.x, in.jitterOffset.y, in.reset ? 1 : 0, in.frameTimeMs);
            fclose(f);
        }
    }
    ++m_EvalIndex;

    ffxContext context = reinterpret_cast<ffxContext>(m_Context);
    if (ffx::Dispatch(context, desc) != ffx::ReturnCode::Ok)
        donut::log::error("NSS dispatch failed");

    // The SDK recorded raw Vulkan commands; make nvrhi re-emit its bindings.
    commandList->clearState();
}

// Loads one recorded texture (header {width, height} + tightly packed rows, as the capture's dumpTexture writes it).
static bool LoadRecorded(const std::string& path, uint32_t bytesPerPixel, uint32_t& width, uint32_t& height, std::vector<uint8_t>& data)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    uint32_t header[2] = {};
    bool     ok        = fread(header, sizeof(header), 1, f) == 1;
    width              = header[0];
    height             = header[1];
    data.resize(size_t(width) * height * bytesPerPixel);
    ok = ok && fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

bool NSSWrapper::ReplayInputs(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, const Inputs& in, Inputs& out)
{
    const char* dir = getenv("NSS_REPLAY_DIR");
    if (!dir)
        return false;
    char name[64];
    auto path = [&](const char* prefix) {
        snprintf(name, sizeof(name), "/%s_%04u.bin", prefix, m_EvalIndex);
        return std::string(dir) + name;
    };

    // Parameters: the line for m_EvalIndex in params.txt.
    FILE* pf = fopen((std::string(dir) + "/params.txt").c_str(), "r");
    if (!pf)
        return false;
    bool     found = false;
    unsigned idx   = 0;
    float    jx = 0, jy = 0, frameTime = 0;
    int      reset = 0;
    while (fscanf(pf, "%u %f %f %d %f", &idx, &jx, &jy, &reset, &frameTime) == 5)
    {
        if (idx == m_EvalIndex)
        {
            found = true;
            break;
        }
    }
    fclose(pf);

    std::vector<uint8_t> color, motion, depth;
    uint32_t             w = 0, h = 0, mw = 0, mh = 0, dw = 0, dh = 0;
    if (!found || !LoadRecorded(path("input"), 8, w, h, color) || !LoadRecorded(path("mv"), 4, mw, mh, motion) ||
        !LoadRecorded(path("depth"), 4, dw, dh, depth))
    {
        donut::log::warning("NSS replay: no recorded frame %u in %s", m_EvalIndex, dir);
        return false;
    }

    auto ensure = [&](nvrhi::TextureHandle& tex, nvrhi::ITexture* like, const char* debugName) {
        const nvrhi::TextureDesc& ld = like->getDesc();
        if (tex && tex->getDesc().width == ld.width && tex->getDesc().height == ld.height && tex->getDesc().format == ld.format)
            return;
        nvrhi::TextureDesc desc = ld;
        desc.isRenderTarget     = false;
        desc.isUAV              = false;
        desc.isVirtual          = false;
        desc.useClearValue      = false;
        desc.sampleCount        = 1;
        desc.dimension          = nvrhi::TextureDimension::Texture2D;
        desc.initialState       = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState   = true;
        desc.debugName          = debugName;
        tex                     = device->createTexture(desc);
    };
    ensure(m_ReplayColor, in.color, "NSS replay colour");
    ensure(m_ReplayMotion, in.motionVectors, "NSS replay motion");
    commandList->writeTexture(m_ReplayColor, 0, 0, color.data(), size_t(w) * 8);
    commandList->writeTexture(m_ReplayMotion, 0, 0, motion.data(), size_t(mw) * 4);
    commandList->writeTexture(m_DepthR32, 0, 0, depth.data(), size_t(dw) * 4);

    out               = in;
    out.color         = m_ReplayColor;
    out.motionVectors = m_ReplayMotion;
    out.jitterOffset  = {jx, jy};
    out.reset         = reset != 0;
    out.frameTimeMs   = frameTime;
    return true;
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
    m_ReplayColor       = nullptr;
    m_ReplayMotion      = nullptr;
}

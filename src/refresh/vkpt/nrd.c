#include "nrd.h"

#ifdef CONFIG_USE_NRD

#include "vkpt.h"
#include "vk_util.h"
#include <NRD.h>
#include <NRDSettings.h>

// CVars
cvar_t* cvar_pt_nrd_enable = NULL;
cvar_t* cvar_pt_nrd_blur_radius = NULL;
cvar_t* cvar_pt_nrd_accumulation = NULL;

// NRD state
static struct {
    nrd_Instance* instance;
    qboolean initialized;
    qboolean resources_created;

    // Resolution
    uint32_t width;
    uint32_t height;

    // Vulkan pipelines created from NRD SPIRV
    VkPipeline pipelines[NRD_MAX_PIPELINES];
    VkPipelineLayout pipeline_layout;
    VkDescriptorSetLayout descriptor_set_layout;
    uint32_t pipeline_count;

    // Descriptor pools and sets
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_sets[NRD_QUEUED_FRAMES];

    // Samplers
    VkSampler sampler_nearest;
    VkSampler sampler_linear;

    // Texture pools
    VkImage permanent_pool[NRD_MAX_PERMANENT_TEXTURES];
    VkImageView permanent_pool_views[NRD_MAX_PERMANENT_TEXTURES];
    VkDeviceMemory permanent_pool_memory[NRD_MAX_PERMANENT_TEXTURES];
    uint32_t permanent_pool_size;

    VkImage transient_pool[NRD_MAX_TRANSIENT_TEXTURES];
    VkImageView transient_pool_views[NRD_MAX_TRANSIENT_TEXTURES];
    VkDeviceMemory transient_pool_memory[NRD_MAX_TRANSIENT_TEXTURES];
    uint32_t transient_pool_size;

    // Constant buffer
    VkBuffer constant_buffer;
    VkDeviceMemory constant_buffer_memory;
    void* constant_buffer_mapped;
    uint32_t constant_buffer_size;

    // Denoiser identifier
    nrd_Identifier denoiser_id;

} nrd_state;

// Forward declarations
static VkFormat nrd_format_to_vk(nrd_Format format);
static qboolean create_samplers(void);
static qboolean create_pipeline_layout(void);
static qboolean create_pipelines(const nrd_InstanceDesc* desc);
static qboolean create_texture_pools(const nrd_InstanceDesc* desc);
static qboolean create_constant_buffer(const nrd_InstanceDesc* desc);
static void destroy_samplers(void);
static void destroy_pipeline_layout(void);
static void destroy_pipelines(void);
static void destroy_texture_pools(void);
static void destroy_constant_buffer(void);

void vkpt_nrd_init_cvars(void)
{
    cvar_pt_nrd_enable = Cvar_Get("pt_nrd_enable", "0", CVAR_ARCHIVE);
    cvar_pt_nrd_blur_radius = Cvar_Get("pt_nrd_blur_radius", "8", CVAR_ARCHIVE);
    cvar_pt_nrd_accumulation = Cvar_Get("pt_nrd_accumulation", "6", CVAR_ARCHIVE);
}

qboolean vkpt_nrd_init(void)
{
    memset(&nrd_state, 0, sizeof(nrd_state));

    vkpt_nrd_init_cvars();

    // Get library description to check SPIRV offsets
    const nrd_LibraryDesc* lib_desc = nrd_GetLibraryDesc();
    if (!lib_desc) {
        Com_EPrintf("NRD: Failed to get library description\n");
        return qfalse;
    }

    Com_Printf("NRD: Library version %d.%d.%d\n",
        lib_desc->versionMajor, lib_desc->versionMinor, lib_desc->versionBuild);
    Com_Printf("NRD: Normal encoding: %d, Roughness encoding: %d\n",
        lib_desc->normalEncoding, lib_desc->roughnessEncoding);
    Com_Printf("NRD: SPIRV offsets - sampler: %u, texture: %u, cbuffer: %u, storage: %u\n",
        lib_desc->spirvBindingOffsets.samplerOffset,
        lib_desc->spirvBindingOffsets.textureOffset,
        lib_desc->spirvBindingOffsets.constantBufferOffset,
        lib_desc->spirvBindingOffsets.storageTextureAndBufferOffset);

    // Create samplers
    if (!create_samplers()) {
        Com_EPrintf("NRD: Failed to create samplers\n");
        return qfalse;
    }

    nrd_state.initialized = qtrue;
    Com_Printf("NRD: Initialized successfully\n");

    return qtrue;
}

void vkpt_nrd_destroy(void)
{
    if (!nrd_state.initialized)
        return;

    vkpt_nrd_destroy_resources();
    destroy_samplers();

    nrd_state.initialized = qfalse;
    Com_Printf("NRD: Destroyed\n");
}

qboolean vkpt_nrd_create_resources(uint32_t width, uint32_t height)
{
    if (!nrd_state.initialized) {
        Com_EPrintf("NRD: Not initialized\n");
        return qfalse;
    }

    // Destroy existing resources if any
    vkpt_nrd_destroy_resources();

    nrd_state.width = width;
    nrd_state.height = height;

    // Define denoiser to create
    nrd_DenoiserDesc denoiser_descs[1];
    denoiser_descs[0].identifier = 0;  // Use 0 as our denoiser ID
    denoiser_descs[0].denoiser = nrd_Denoiser_REBLUR_DIFFUSE_SPECULAR;
    nrd_state.denoiser_id = 0;

    // Create NRD instance
    nrd_InstanceCreationDesc creation_desc;
    memset(&creation_desc, 0, sizeof(creation_desc));
    creation_desc.denoisers = denoiser_descs;
    creation_desc.denoisersNum = 1;
    // Use default allocation callbacks (NULL = malloc/free)

    nrd_Result result = nrd_CreateInstance(&creation_desc, &nrd_state.instance);
    if (result != nrd_Result_SUCCESS) {
        Com_EPrintf("NRD: Failed to create instance: %d\n", result);
        return qfalse;
    }

    // Get instance description
    const nrd_InstanceDesc* inst_desc = nrd_GetInstanceDesc(nrd_state.instance);
    if (!inst_desc) {
        Com_EPrintf("NRD: Failed to get instance description\n");
        nrd_DestroyInstance(*nrd_state.instance);
        nrd_state.instance = NULL;
        return qfalse;
    }

    Com_Printf("NRD: Creating resources for %dx%d\n", width, height);
    Com_Printf("NRD: %u pipelines, %u permanent textures, %u transient textures\n",
        inst_desc->pipelinesNum, inst_desc->permanentPoolSize, inst_desc->transientPoolSize);

    // Create Vulkan resources
    if (!create_pipeline_layout()) {
        Com_EPrintf("NRD: Failed to create pipeline layout\n");
        goto fail;
    }

    if (!create_pipelines(inst_desc)) {
        Com_EPrintf("NRD: Failed to create pipelines\n");
        goto fail;
    }

    if (!create_texture_pools(inst_desc)) {
        Com_EPrintf("NRD: Failed to create texture pools\n");
        goto fail;
    }

    if (!create_constant_buffer(inst_desc)) {
        Com_EPrintf("NRD: Failed to create constant buffer\n");
        goto fail;
    }

    nrd_state.resources_created = qtrue;
    Com_Printf("NRD: Resources created successfully\n");

    return qtrue;

fail:
    vkpt_nrd_destroy_resources();
    return qfalse;
}

void vkpt_nrd_destroy_resources(void)
{
    if (!nrd_state.resources_created && !nrd_state.instance)
        return;

    vkDeviceWaitIdle(qvk.device);

    destroy_constant_buffer();
    destroy_texture_pools();
    destroy_pipelines();
    destroy_pipeline_layout();

    if (nrd_state.instance) {
        nrd_DestroyInstance(*nrd_state.instance);
        nrd_state.instance = NULL;
    }

    nrd_state.resources_created = qfalse;
}

void vkpt_nrd_denoise(VkCommandBuffer cmd_buf, uint32_t frame_num)
{
    if (!nrd_state.resources_created || !vkpt_nrd_enabled())
        return;

    // Set common settings
    nrd_CommonSettings common_settings;
    memset(&common_settings, 0, sizeof(common_settings));

    // Copy view matrices from global UBO
    // Note: These need to be transposed/converted to NRD's expected format
    // For now using identity matrices - needs proper matrix setup
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    memcpy(common_settings.viewToClipMatrix, identity, sizeof(identity));
    memcpy(common_settings.viewToClipMatrixPrev, identity, sizeof(identity));
    memcpy(common_settings.worldToViewMatrix, identity, sizeof(identity));
    memcpy(common_settings.worldToViewMatrixPrev, identity, sizeof(identity));
    memcpy(common_settings.worldPrevToWorldMatrix, identity, sizeof(identity));

    common_settings.motionVectorScale[0] = 1.0f;
    common_settings.motionVectorScale[1] = 1.0f;
    common_settings.motionVectorScale[2] = 1.0f;

    common_settings.resourceSize[0] = nrd_state.width;
    common_settings.resourceSize[1] = nrd_state.height;
    common_settings.resourceSizePrev[0] = nrd_state.width;
    common_settings.resourceSizePrev[1] = nrd_state.height;

    common_settings.rectSize[0] = nrd_state.width;
    common_settings.rectSize[1] = nrd_state.height;
    common_settings.rectSizePrev[0] = nrd_state.width;
    common_settings.rectSizePrev[1] = nrd_state.height;

    common_settings.denoisingRange = 100000.0f;
    common_settings.frameIndex = frame_num;
    common_settings.accumulationMode = nrd_AccumulationMode_CONTINUE;
    common_settings.isMotionVectorInWorldSpace = false;

    nrd_Result result = nrd_SetCommonSettings(*nrd_state.instance, &common_settings);
    if (result != nrd_Result_SUCCESS) {
        Com_EPrintf("NRD: Failed to set common settings: %d\n", result);
        return;
    }

    // Set REBLUR settings
    nrd_ReblurSettings reblur_settings;
    memset(&reblur_settings, 0, sizeof(reblur_settings));

    // Conservative settings for post-RR cleanup
    reblur_settings.maxAccumulatedFrameNum = cvar_pt_nrd_accumulation->integer;
    reblur_settings.maxFastAccumulatedFrameNum = 2;
    reblur_settings.diffusePrepassBlurRadius = 0;
    reblur_settings.specularPrepassBlurRadius = 0;
    reblur_settings.minBlurRadius = 2.0f;
    reblur_settings.maxBlurRadius = (float)cvar_pt_nrd_blur_radius->integer;
    reblur_settings.lobeAngleFraction = 0.5f;
    reblur_settings.hitDistanceReconstructionMode = nrd_HitDistanceReconstructionMode_OFF;
    reblur_settings.enableAntiFirefly = true;

    result = nrd_SetDenoiserSettings(*nrd_state.instance, nrd_state.denoiser_id, &reblur_settings);
    if (result != nrd_Result_SUCCESS) {
        Com_EPrintf("NRD: Failed to set denoiser settings: %d\n", result);
        return;
    }

    // Get compute dispatches
    const nrd_DispatchDesc* dispatch_descs = NULL;
    uint32_t dispatch_descs_num = 0;

    nrd_Identifier identifiers[1] = { nrd_state.denoiser_id };
    result = nrd_GetComputeDispatches(*nrd_state.instance, identifiers, 1, &dispatch_descs, &dispatch_descs_num);
    if (result != nrd_Result_SUCCESS) {
        Com_EPrintf("NRD: Failed to get compute dispatches: %d\n", result);
        return;
    }

    // Execute dispatches
    // TODO: Implement actual Vulkan dispatch execution
    // This requires:
    // 1. For each dispatch:
    //    a. Bind the appropriate pipeline
    //    b. Update descriptor sets with input/output textures
    //    c. Upload constant buffer data
    //    d. vkCmdDispatch()

    // For now, just log that we would execute dispatches
    // Full implementation requires significant Vulkan descriptor management

    (void)dispatch_descs;
    (void)dispatch_descs_num;
}

qboolean vkpt_nrd_enabled(void)
{
    return cvar_pt_nrd_enable && cvar_pt_nrd_enable->integer != 0 && nrd_state.resources_created;
}

// Helper function to convert NRD format to Vulkan format
static VkFormat nrd_format_to_vk(nrd_Format format)
{
    switch (format) {
        case nrd_Format_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case nrd_Format_R8_SNORM: return VK_FORMAT_R8_SNORM;
        case nrd_Format_R8_UINT: return VK_FORMAT_R8_UINT;
        case nrd_Format_R8_SINT: return VK_FORMAT_R8_SINT;
        case nrd_Format_RG8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case nrd_Format_RG8_SNORM: return VK_FORMAT_R8G8_SNORM;
        case nrd_Format_RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case nrd_Format_RGBA8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
        case nrd_Format_R16_UNORM: return VK_FORMAT_R16_UNORM;
        case nrd_Format_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case nrd_Format_R16_SFLOAT: return VK_FORMAT_R16_SFLOAT;
        case nrd_Format_RG16_UNORM: return VK_FORMAT_R16G16_UNORM;
        case nrd_Format_RG16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case nrd_Format_RG16_SFLOAT: return VK_FORMAT_R16G16_SFLOAT;
        case nrd_Format_RGBA16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
        case nrd_Format_RGBA16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case nrd_Format_RGBA16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case nrd_Format_R32_UINT: return VK_FORMAT_R32_UINT;
        case nrd_Format_R32_SINT: return VK_FORMAT_R32_SINT;
        case nrd_Format_R32_SFLOAT: return VK_FORMAT_R32_SFLOAT;
        case nrd_Format_RG32_UINT: return VK_FORMAT_R32G32_UINT;
        case nrd_Format_RG32_SINT: return VK_FORMAT_R32G32_SINT;
        case nrd_Format_RG32_SFLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case nrd_Format_RGBA32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
        case nrd_Format_RGBA32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
        case nrd_Format_RGBA32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case nrd_Format_R10_G10_B10_A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case nrd_Format_R11_G11_B10_UFLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        default: return VK_FORMAT_UNDEFINED;
    }
}

static qboolean create_samplers(void)
{
    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };

    if (vkCreateSampler(qvk.device, &sampler_info, NULL, &nrd_state.sampler_nearest) != VK_SUCCESS) {
        return qfalse;
    }

    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(qvk.device, &sampler_info, NULL, &nrd_state.sampler_linear) != VK_SUCCESS) {
        vkDestroySampler(qvk.device, nrd_state.sampler_nearest, NULL);
        nrd_state.sampler_nearest = VK_NULL_HANDLE;
        return qfalse;
    }

    return qtrue;
}

static void destroy_samplers(void)
{
    if (nrd_state.sampler_nearest) {
        vkDestroySampler(qvk.device, nrd_state.sampler_nearest, NULL);
        nrd_state.sampler_nearest = VK_NULL_HANDLE;
    }
    if (nrd_state.sampler_linear) {
        vkDestroySampler(qvk.device, nrd_state.sampler_linear, NULL);
        nrd_state.sampler_linear = VK_NULL_HANDLE;
    }
}

static qboolean create_pipeline_layout(void)
{
    // NRD uses:
    // - 1 constant buffer (push constants or UBO)
    // - 2 samplers (nearest, linear)
    // - N input textures (SRV)
    // - M output textures (UAV)

    // For simplicity, we'll use a single descriptor set layout that can handle
    // the maximum resources NRD might need

    // Binding 0: Constant buffer (or use push constants)
    // Binding 1-2: Samplers
    // Binding 3+: Textures (combined SRV and UAV)

    VkDescriptorSetLayoutBinding bindings[64];
    uint32_t binding_count = 0;

    // Samplers
    bindings[binding_count++] = (VkDescriptorSetLayoutBinding) {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
        .descriptorCount = 2,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };

    // Textures (read-only)
    bindings[binding_count++] = (VkDescriptorSetLayoutBinding) {
        .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .descriptorCount = 32,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };

    // Storage textures (read-write)
    bindings[binding_count++] = (VkDescriptorSetLayoutBinding) {
        .binding = 2,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 32,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = binding_count,
        .pBindings = bindings,
    };

    if (vkCreateDescriptorSetLayout(qvk.device, &layout_info, NULL, &nrd_state.descriptor_set_layout) != VK_SUCCESS) {
        return qfalse;
    }

    // Create pipeline layout with push constants for the constant buffer
    VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = 256, // NRD constant buffer is typically < 256 bytes
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &nrd_state.descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant_range,
    };

    if (vkCreatePipelineLayout(qvk.device, &pipeline_layout_info, NULL, &nrd_state.pipeline_layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(qvk.device, nrd_state.descriptor_set_layout, NULL);
        nrd_state.descriptor_set_layout = VK_NULL_HANDLE;
        return qfalse;
    }

    return qtrue;
}

static void destroy_pipeline_layout(void)
{
    if (nrd_state.pipeline_layout) {
        vkDestroyPipelineLayout(qvk.device, nrd_state.pipeline_layout, NULL);
        nrd_state.pipeline_layout = VK_NULL_HANDLE;
    }
    if (nrd_state.descriptor_set_layout) {
        vkDestroyDescriptorSetLayout(qvk.device, nrd_state.descriptor_set_layout, NULL);
        nrd_state.descriptor_set_layout = VK_NULL_HANDLE;
    }
}

static qboolean create_pipelines(const nrd_InstanceDesc* desc)
{
    nrd_state.pipeline_count = desc->pipelinesNum;

    for (uint32_t i = 0; i < desc->pipelinesNum && i < NRD_MAX_PIPELINES; i++) {
        const nrd_PipelineDesc* pipeline_desc = &desc->pipelines[i];

        // Use SPIRV bytecode
        if (pipeline_desc->computeShaderSPIRV.size == 0 || pipeline_desc->computeShaderSPIRV.bytecode == NULL) {
            Com_EPrintf("NRD: Pipeline %u has no SPIRV bytecode\n", i);
            return qfalse;
        }

        VkShaderModuleCreateInfo shader_module_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = pipeline_desc->computeShaderSPIRV.size,
            .pCode = (const uint32_t*)pipeline_desc->computeShaderSPIRV.bytecode,
        };

        VkShaderModule shader_module;
        if (vkCreateShaderModule(qvk.device, &shader_module_info, NULL, &shader_module) != VK_SUCCESS) {
            Com_EPrintf("NRD: Failed to create shader module for pipeline %u\n", i);
            return qfalse;
        }

        VkComputePipelineCreateInfo pipeline_info = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = shader_module,
                .pName = desc->shaderEntryPoint ? desc->shaderEntryPoint : "main",
            },
            .layout = nrd_state.pipeline_layout,
        };

        VkResult result = vkCreateComputePipelines(qvk.device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &nrd_state.pipelines[i]);

        vkDestroyShaderModule(qvk.device, shader_module, NULL);

        if (result != VK_SUCCESS) {
            Com_EPrintf("NRD: Failed to create pipeline %u\n", i);
            return qfalse;
        }
    }

    Com_Printf("NRD: Created %u pipelines\n", nrd_state.pipeline_count);
    return qtrue;
}

static void destroy_pipelines(void)
{
    for (uint32_t i = 0; i < nrd_state.pipeline_count && i < NRD_MAX_PIPELINES; i++) {
        if (nrd_state.pipelines[i]) {
            vkDestroyPipeline(qvk.device, nrd_state.pipelines[i], NULL);
            nrd_state.pipelines[i] = VK_NULL_HANDLE;
        }
    }
    nrd_state.pipeline_count = 0;
}

static qboolean create_texture_pools(const nrd_InstanceDesc* desc)
{
    // Create permanent texture pool
    nrd_state.permanent_pool_size = desc->permanentPoolSize;
    for (uint32_t i = 0; i < desc->permanentPoolSize && i < NRD_MAX_PERMANENT_TEXTURES; i++) {
        const nrd_TextureDesc* tex_desc = &desc->permanentPool[i];
        VkFormat format = nrd_format_to_vk(tex_desc->format);

        uint32_t width = nrd_state.width / tex_desc->downsampleFactor;
        uint32_t height = nrd_state.height / tex_desc->downsampleFactor;
        if (width < 1) width = 1;
        if (height < 1) height = 1;

        VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = { width, height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        if (vkCreateImage(qvk.device, &image_info, NULL, &nrd_state.permanent_pool[i]) != VK_SUCCESS) {
            Com_EPrintf("NRD: Failed to create permanent pool image %u\n", i);
            return qfalse;
        }

        // Allocate memory
        VkMemoryRequirements mem_req;
        vkGetImageMemoryRequirements(qvk.device, nrd_state.permanent_pool[i], &mem_req);

        VkMemoryAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mem_req.size,
            .memoryTypeIndex = get_memory_type(mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };

        if (vkAllocateMemory(qvk.device, &alloc_info, NULL, &nrd_state.permanent_pool_memory[i]) != VK_SUCCESS) {
            Com_EPrintf("NRD: Failed to allocate permanent pool memory %u\n", i);
            return qfalse;
        }

        vkBindImageMemory(qvk.device, nrd_state.permanent_pool[i], nrd_state.permanent_pool_memory[i], 0);

        // Create image view
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = nrd_state.permanent_pool[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        if (vkCreateImageView(qvk.device, &view_info, NULL, &nrd_state.permanent_pool_views[i]) != VK_SUCCESS) {
            Com_EPrintf("NRD: Failed to create permanent pool view %u\n", i);
            return qfalse;
        }
    }

    // Create transient texture pool (similar to permanent)
    nrd_state.transient_pool_size = desc->transientPoolSize;
    for (uint32_t i = 0; i < desc->transientPoolSize && i < NRD_MAX_TRANSIENT_TEXTURES; i++) {
        const nrd_TextureDesc* tex_desc = &desc->transientPool[i];
        VkFormat format = nrd_format_to_vk(tex_desc->format);

        uint32_t width = nrd_state.width / tex_desc->downsampleFactor;
        uint32_t height = nrd_state.height / tex_desc->downsampleFactor;
        if (width < 1) width = 1;
        if (height < 1) height = 1;

        VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = { width, height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        if (vkCreateImage(qvk.device, &image_info, NULL, &nrd_state.transient_pool[i]) != VK_SUCCESS) {
            Com_EPrintf("NRD: Failed to create transient pool image %u\n", i);
            return qfalse;
        }

        VkMemoryRequirements mem_req;
        vkGetImageMemoryRequirements(qvk.device, nrd_state.transient_pool[i], &mem_req);

        VkMemoryAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mem_req.size,
            .memoryTypeIndex = get_memory_type(mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };

        if (vkAllocateMemory(qvk.device, &alloc_info, NULL, &nrd_state.transient_pool_memory[i]) != VK_SUCCESS) {
            Com_EPrintf("NRD: Failed to allocate transient pool memory %u\n", i);
            return qfalse;
        }

        vkBindImageMemory(qvk.device, nrd_state.transient_pool[i], nrd_state.transient_pool_memory[i], 0);

        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = nrd_state.transient_pool[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        if (vkCreateImageView(qvk.device, &view_info, NULL, &nrd_state.transient_pool_views[i]) != VK_SUCCESS) {
            Com_EPrintf("NRD: Failed to create transient pool view %u\n", i);
            return qfalse;
        }
    }

    Com_Printf("NRD: Created %u permanent and %u transient pool textures\n",
        nrd_state.permanent_pool_size, nrd_state.transient_pool_size);

    return qtrue;
}

static void destroy_texture_pools(void)
{
    for (uint32_t i = 0; i < nrd_state.permanent_pool_size && i < NRD_MAX_PERMANENT_TEXTURES; i++) {
        if (nrd_state.permanent_pool_views[i]) {
            vkDestroyImageView(qvk.device, nrd_state.permanent_pool_views[i], NULL);
            nrd_state.permanent_pool_views[i] = VK_NULL_HANDLE;
        }
        if (nrd_state.permanent_pool[i]) {
            vkDestroyImage(qvk.device, nrd_state.permanent_pool[i], NULL);
            nrd_state.permanent_pool[i] = VK_NULL_HANDLE;
        }
        if (nrd_state.permanent_pool_memory[i]) {
            vkFreeMemory(qvk.device, nrd_state.permanent_pool_memory[i], NULL);
            nrd_state.permanent_pool_memory[i] = VK_NULL_HANDLE;
        }
    }
    nrd_state.permanent_pool_size = 0;

    for (uint32_t i = 0; i < nrd_state.transient_pool_size && i < NRD_MAX_TRANSIENT_TEXTURES; i++) {
        if (nrd_state.transient_pool_views[i]) {
            vkDestroyImageView(qvk.device, nrd_state.transient_pool_views[i], NULL);
            nrd_state.transient_pool_views[i] = VK_NULL_HANDLE;
        }
        if (nrd_state.transient_pool[i]) {
            vkDestroyImage(qvk.device, nrd_state.transient_pool[i], NULL);
            nrd_state.transient_pool[i] = VK_NULL_HANDLE;
        }
        if (nrd_state.transient_pool_memory[i]) {
            vkFreeMemory(qvk.device, nrd_state.transient_pool_memory[i], NULL);
            nrd_state.transient_pool_memory[i] = VK_NULL_HANDLE;
        }
    }
    nrd_state.transient_pool_size = 0;
}

static qboolean create_constant_buffer(const nrd_InstanceDesc* desc)
{
    nrd_state.constant_buffer_size = desc->constantBufferMaxDataSize;

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = nrd_state.constant_buffer_size,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    if (vkCreateBuffer(qvk.device, &buffer_info, NULL, &nrd_state.constant_buffer) != VK_SUCCESS) {
        return qfalse;
    }

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(qvk.device, nrd_state.constant_buffer, &mem_req);

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = get_memory_type(mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };

    if (vkAllocateMemory(qvk.device, &alloc_info, NULL, &nrd_state.constant_buffer_memory) != VK_SUCCESS) {
        vkDestroyBuffer(qvk.device, nrd_state.constant_buffer, NULL);
        nrd_state.constant_buffer = VK_NULL_HANDLE;
        return qfalse;
    }

    vkBindBufferMemory(qvk.device, nrd_state.constant_buffer, nrd_state.constant_buffer_memory, 0);
    vkMapMemory(qvk.device, nrd_state.constant_buffer_memory, 0, nrd_state.constant_buffer_size, 0, &nrd_state.constant_buffer_mapped);

    return qtrue;
}

static void destroy_constant_buffer(void)
{
    if (nrd_state.constant_buffer_memory) {
        vkUnmapMemory(qvk.device, nrd_state.constant_buffer_memory);
        vkFreeMemory(qvk.device, nrd_state.constant_buffer_memory, NULL);
        nrd_state.constant_buffer_memory = VK_NULL_HANDLE;
        nrd_state.constant_buffer_mapped = NULL;
    }
    if (nrd_state.constant_buffer) {
        vkDestroyBuffer(qvk.device, nrd_state.constant_buffer, NULL);
        nrd_state.constant_buffer = VK_NULL_HANDLE;
    }
    nrd_state.constant_buffer_size = 0;
}

#endif // CONFIG_USE_NRD

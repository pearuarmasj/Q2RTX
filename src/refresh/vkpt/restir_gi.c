/*
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

// ========================================================================== //
// ReSTIR GI Pipeline Management
//
// Handles the creation, destruction, and execution of compute shaders for
// ReSTIR GI (Global Illumination) temporal and spatial resampling.
// ========================================================================== //

#include "vkpt.h"

// External cvar declarations for ReSTIR GI settings
extern cvar_t *cvar_pt_restir_gi_enable;
extern cvar_t *cvar_pt_restir_gi_temporal;
extern cvar_t *cvar_pt_restir_gi_spatial;
extern cvar_t *cvar_pt_restir_gi_spatial_neighbors;
extern cvar_t *cvar_pt_restir_gi_max_history;
extern cvar_t *cvar_pt_restir_gi_max_reservoir_age;
extern cvar_t *cvar_pt_restir_gi_depth_threshold;
extern cvar_t *cvar_pt_restir_gi_normal_threshold;
extern cvar_t *cvar_pt_restir_gi_spatial_radius;
extern cvar_t *cvar_pt_restir_gi_debug_view;

enum {
    RESTIR_GI_TEMPORAL,
    RESTIR_GI_SPATIAL,
    RESTIR_GI_APPLY,
    RESTIR_GI_NUM_PIPELINES
};

static VkPipeline       pipeline_restir_gi[RESTIR_GI_NUM_PIPELINES];
static VkPipelineLayout pipeline_layout_restir_gi;

VkResult
vkpt_restir_gi_initialize(void)
{
    VkDescriptorSetLayout desc_set_layouts[] = {
        qvk.desc_set_layout_ubo,
        qvk.desc_set_layout_textures,
        qvk.desc_set_layout_vertex_buffer
    };

    CREATE_PIPELINE_LAYOUT(qvk.device, &pipeline_layout_restir_gi,
        .setLayoutCount = LENGTH(desc_set_layouts),
        .pSetLayouts = desc_set_layouts,
    );
    ATTACH_LABEL_VARIABLE(pipeline_layout_restir_gi, PIPELINE_LAYOUT);

    return VK_SUCCESS;
}

VkResult
vkpt_restir_gi_destroy(void)
{
    vkDestroyPipelineLayout(qvk.device, pipeline_layout_restir_gi, NULL);
    return VK_SUCCESS;
}

VkResult
vkpt_restir_gi_create_pipelines(void)
{
    VkComputePipelineCreateInfo pipeline_info[RESTIR_GI_NUM_PIPELINES] = {
        [RESTIR_GI_TEMPORAL] = {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = SHADER_STAGE(QVK_MOD_RESTIR_GI_TEMPORAL_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
            .layout = pipeline_layout_restir_gi,
        },
        [RESTIR_GI_SPATIAL] = {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = SHADER_STAGE(QVK_MOD_RESTIR_GI_SPATIAL_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
            .layout = pipeline_layout_restir_gi,
        },
        [RESTIR_GI_APPLY] = {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = SHADER_STAGE(QVK_MOD_RESTIR_GI_APPLY_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
            .layout = pipeline_layout_restir_gi,
        },
    };

    _VK(vkCreateComputePipelines(qvk.device, 0, LENGTH(pipeline_info), pipeline_info, 0, pipeline_restir_gi));

    return VK_SUCCESS;
}

VkResult
vkpt_restir_gi_destroy_pipelines(void)
{
    for (int i = 0; i < RESTIR_GI_NUM_PIPELINES; i++)
        vkDestroyPipeline(qvk.device, pipeline_restir_gi[i], NULL);
    return VK_SUCCESS;
}

#define BARRIER_COMPUTE(cmd_buf, img) \
    do { \
        VkImageSubresourceRange subresource_range = { \
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT, \
            .baseMipLevel   = 0, \
            .levelCount     = 1, \
            .baseArrayLayer = 0, \
            .layerCount     = 1 \
        }; \
        IMAGE_BARRIER(cmd_buf, \
                .image            = img, \
                .subresourceRange = subresource_range, \
                .srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT, \
                .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT, \
                .oldLayout        = VK_IMAGE_LAYOUT_GENERAL, \
                .newLayout        = VK_IMAGE_LAYOUT_GENERAL, \
        ); \
    } while(0)

VkResult
vkpt_restir_gi_record_cmd_buffer(VkCommandBuffer cmd_buf)
{
    // Check if ReSTIR GI is enabled (debug view 99 always runs for testing)
    int debug_view = (int)(cvar_pt_restir_gi_debug_view->value + 0.5f);
    if (cvar_pt_restir_gi_enable->value == 0 && debug_view != 99)
        return VK_SUCCESS;

    VkDescriptorSet desc_sets[] = {
        qvk.desc_set_ubo,
        qvk_get_current_desc_set_textures(),
        qvk.desc_set_vertex_buffer
    };

    int frame_idx = qvk.frame_counter & 1;

    // Barrier for initial GI reservoir data written by indirect_lighting.rgen
    BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_POS_A + frame_idx]);
    BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_NORM_RAD_A + frame_idx]);
    BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_WEIGHT_A + frame_idx]);

    // -------------------------------------------------------------------------
    // Temporal Resampling Pass
    // -------------------------------------------------------------------------
    
    BEGIN_PERF_MARKER(cmd_buf, PROFILER_ASVGF_FULL);  // Use existing profiler marker for now
    
    vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_restir_gi[RESTIR_GI_TEMPORAL]);
    vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipeline_layout_restir_gi, 0, LENGTH(desc_sets), desc_sets, 0, 0);

    uint32_t group_size = 16;
    vkCmdDispatch(cmd_buf,
        (qvk.extent_render.width + group_size - 1) / group_size,
        (qvk.extent_render.height + group_size - 1) / group_size,
        1);

    // Barrier after temporal pass
    BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_POS_A + frame_idx]);
    BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_NORM_RAD_A + frame_idx]);
    BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_WEIGHT_A + frame_idx]);

    // -------------------------------------------------------------------------
    // Spatial Resampling Pass
    // -------------------------------------------------------------------------
    
    if (cvar_pt_restir_gi_spatial->value != 0)
    {
        vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_restir_gi[RESTIR_GI_SPATIAL]);
        vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline_layout_restir_gi, 0, LENGTH(desc_sets), desc_sets, 0, 0);

        vkCmdDispatch(cmd_buf,
            (qvk.extent_render.width + group_size - 1) / group_size,
            (qvk.extent_render.height + group_size - 1) / group_size,
            1);

        // Barrier after spatial pass
        BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_POS_A + frame_idx]);
        BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_NORM_RAD_A + frame_idx]);
        BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_WEIGHT_A + frame_idx]);
    }

    // -------------------------------------------------------------------------
    // Apply Pass - Adds resampled GI to lighting output
    // -------------------------------------------------------------------------
    
    vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_restir_gi[RESTIR_GI_APPLY]);
    vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipeline_layout_restir_gi, 0, LENGTH(desc_sets), desc_sets, 0, 0);

    vkCmdDispatch(cmd_buf,
        (qvk.extent_render.width + group_size - 1) / group_size,
        (qvk.extent_render.height + group_size - 1) / group_size,
        1);

    // Final barrier for color buffers modified by apply pass
    BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_LF_SH]);
    BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_LF_COCG]);

    END_PERF_MARKER(cmd_buf, PROFILER_ASVGF_FULL);

    return VK_SUCCESS;
}

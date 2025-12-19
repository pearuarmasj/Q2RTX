# DLSS Ray Reconstruction Implementation Plan

## Status: COMPLETED

## Overview

DLSS Ray Reconstruction (RR) is a denoiser that replaces ASVGF. It requires raw path tracer outputs including:
- Noisy color (diffuse + specular, pre-denoise)
- G-buffer data (albedo, roughness, metallic, normals, etc.)
- Ray hit distances (diffuse and specular)
- Motion vectors (2D screen-space, 3D world-space, reflection)
- Transparency mask
- Before-transparent color (for proper transparency compositing)

## Completed Implementation

### Task 1: PT_ Images Added (DONE)
- Added 16 new PT_ images to `global_textures.h` (indices 59-74)
- Updated `NUM_IMAGES_BASE` from 59 to 75

### Task 2: Shader Updates (DONE)
- `primary_rays.rgen`: Writes PT_ALBEDO, PT_ROUGHNESS, PT_METALLIC_RR, PT_NORMAL_RR, PT_VIEW_DEPTH_SPLIT
- `direct_lighting.rgen`: Writes PT_SPECULAR, PT_MATERIALID, PT_SPECULAR_ALBEDO
- `compositing.comp`: Writes PT_BEFORE_TRANSPARENT
- `brdf.glsl`: Added `composite_color_without_transparent()` function

### Task 3: Buffer Clear Function (DONE)
- Added `vkpt_dlss_rr_clear_buffers()` in main.c
- Called after PROFILER_FRAME_TIME START

### Task 4: Buffer Copy Function (DONE)
- Added `vkpt_dlss_rr_copy_buffers()` in main.c
- Copies PT_ buffers to DLSS_ buffers using `vkpt_image_copy()`
- Called before DLSSApply()

### Task 5: DLSS-RR Evaluation (Already Existed)
- DLSS.c `DLSSApply()` already uses `NGX_VULKAN_EVALUATE_DLSSD_EXT` when `pt_dlss_dldn=1`
- All DLSS_ buffers correctly referenced

## Data Flow

```
Path Tracer (render res) --> PT_ buffers --> copy --> DLSS_ buffers --> DLSS-RR NGX --> DLSS_OUTPUT
```

## Testing

Enable Ray Reconstruction:
```
pt_dlss 1
pt_dlss_dldn 1
```

Debug visualization (pt_dlss_debug):
- 4: DLSS_ALBEDO
- 5: DLSS_SPECULAR
- 6: DLSS_ROUGHNESS
- 7: DLSS_METALLIC
- 8: DLSS_NORMAL

---

## Original Plan (for reference)

**Note:** Indices XX need to be sequential starting after current last image (58). Update `NUM_IMAGES_BASE` accordingly.

---

### Task 2: Update Shaders to Write PT_ Buffers

#### 2a. primary_rays.rgen

**File:** `src/refresh/vkpt/shader/primary_rays.rgen`

**Add writes near the end of main()**, after surface parameters are computed:

```glsl
// Ray Reconstruction data outputs
imageStore(IMG_PT_ALBEDO, ipos, vec4(primary_albedo, 0));
imageStore(IMG_PT_ROUGHNESS, ipos, vec4(primary_roughness, 0, 0, 0));
imageStore(IMG_PT_METALLIC, ipos, vec4(primary_metallic, 0, 0, 0));
imageStore(IMG_PT_NORMAL, ipos, vec4(normal, 0));
```

Reference implementation writes at line ~328-338 in primary_rays.rgen.

#### 2b. direct_lighting.rgen

**File:** `src/refresh/vkpt/shader/direct_lighting.rgen`

**Add writes after lighting computation:**

```glsl
// RR data - specular factor and normals
imageStore(IMG_PT_SPECULAR, ipos, vec4(primary_specular_factor, 0, 0, 0));
imageStore(IMG_PT_NORMAL, ipos, vec4(normal, 0));
imageStore(IMG_PT_SPECULAR_ALBEDO, ipos, vec4(primary_base_reflectivity, 0));
```

Reference implementation writes at lines ~328-345, ~430-465 in direct_lighting.rgen.

#### 2c. reflect_refract.rgen

**File:** `src/refresh/vkpt/shader/reflect_refract.rgen`

Add writes for:
- `IMG_PT_REFLECTED_ALBEDO` - reflected surface albedo
- `IMG_PT_RAYLENGTH_SPECULAR` - specular ray hit distance

#### 2d. indirect_lighting.rgen (if GI rays write hit distance)

**File:** `src/refresh/vkpt/shader/indirect_lighting.rgen`

Add writes for:
- `IMG_PT_RAYLENGTH_DIFFUSE` - diffuse bounce ray hit distance

#### 2e. compositing.comp

**File:** `src/refresh/vkpt/shader/compositing.comp`

Add writes for:
- `IMG_PT_BEFORE_TRANSPARENT` - color before transparency composite

---

### Task 3: Add PT_ to DLSS_ Copy/Resample in main.c

**File:** `src/refresh/vkpt/main.c`

**Location:** After denoiser pass, before `DLSSApply()` call

The PT_ buffers are at render resolution (which may be scaled down by DLSS preset).
The DLSS_ buffers are at DLSS input resolution.

If render resolution == DLSS input resolution (typical), use `vkpt_image_copy()`.
If different, use `vkCmdBlitImage()` with appropriate filter.

**Add function or inline code:**

```c
static void copy_pt_to_dlss_buffers(VkCommandBuffer cmd_buf)
{
    // Only needed when DLSS Ray Reconstruction is enabled
    if (!DLSSEnabled() || DLSSModeDenoise() != 1)
        return;
    
    VkOffset2D zero_offset = { 0, 0 };
    VkExtent2D render_extent = qvk.extent_render;
    
    // Copy each PT_ buffer to corresponding DLSS_ buffer
    vkpt_image_copy(cmd_buf, VKPT_IMG_PT_ALBEDO, VKPT_IMG_DLSS_ALBEDO, 
                    zero_offset, zero_offset, render_extent);
    vkpt_image_copy(cmd_buf, VKPT_IMG_PT_ROUGHNESS, VKPT_IMG_DLSS_ROUGHNESS,
                    zero_offset, zero_offset, render_extent);
    vkpt_image_copy(cmd_buf, VKPT_IMG_PT_METALLIC, VKPT_IMG_DLSS_METALLIC,
                    zero_offset, zero_offset, render_extent);
    vkpt_image_copy(cmd_buf, VKPT_IMG_PT_NORMAL, VKPT_IMG_DLSS_NORMAL,
                    zero_offset, zero_offset, render_extent);
    vkpt_image_copy(cmd_buf, VKPT_IMG_PT_SPECULAR, VKPT_IMG_DLSS_SPECULAR,
                    zero_offset, zero_offset, render_extent);
    vkpt_image_copy(cmd_buf, VKPT_IMG_PT_EMISSIVE, VKPT_IMG_DLSS_EMISSIVE,
                    zero_offset, zero_offset, render_extent);
    vkpt_image_copy(cmd_buf, VKPT_IMG_PT_INDIRECT_ALBEDO, VKPT_IMG_DLSS_INDIRECT_ALBEDO,
                    zero_offset, zero_offset, render_extent);
    vkpt_image_copy(cmd_buf, VKPT_IMG_PT_SPECULAR_ALBEDO, VKPT_IMG_DLSS_SPECULAR_ALBEDO,
                    zero_offset, zero_offset, render_extent);
    vkpt_image_copy(cmd_buf, VKPT_IMG_PT_BEFORE_TRANSPARENT, VKPT_IMG_DLSS_BEFORE_TRANSPARENT,
                    zero_offset, zero_offset, render_extent);
    vkpt_image_copy(cmd_buf, VKPT_IMG_PT_RAYLENGTH_DIFFUSE, VKPT_IMG_DLSS_RAYLENGTH_DIFFUSE,
                    zero_offset, zero_offset, render_extent);
    vkpt_image_copy(cmd_buf, VKPT_IMG_PT_RAYLENGTH_SPECULAR, VKPT_IMG_DLSS_RAYLENGTH_SPECULAR,
                    zero_offset, zero_offset, render_extent);
    vkpt_image_copy(cmd_buf, VKPT_IMG_PT_REFLECTED_ALBEDO, VKPT_IMG_DLSS_REFLECTED_ALBEDO,
                    zero_offset, zero_offset, render_extent);
}
```

**Call location:** Before `DLSSApply()` in the render function.

---

### Task 4: Clear PT_ Buffers Each Frame

**File:** `src/refresh/vkpt/main.c`

**Location:** Beginning of frame rendering (near line ~3050-3260 in reference)

Add clearing for all PT_ buffers:

```c
const VkClearColorValue emptyColor = {
    .float32[0] = 0.0f,
    .float32[1] = 0.0f,
    .float32[2] = 0.0f,
    .float32[3] = 0.0f
};

const VkImageSubresourceRange subresource_range = {
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .levelCount = 1,
    .layerCount = 1
};

// Clear RR source buffers
vkCmdClearColorImage(cmd_buf, qvk.images[VKPT_IMG_PT_ALBEDO], 
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyColor, 1, &subresource_range);
// ... (repeat for each PT_ buffer)

// Transition to GENERAL layout for shader writes
IMAGE_BARRIER(cmd_buf,
    .image = qvk.images[VKPT_IMG_PT_ALBEDO],
    .subresourceRange = subresource_range,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
);
// ... (repeat for each PT_ buffer)
```

---

### Task 5: Verify DLSS-RR Eval Params

**File:** `src/refresh/vkpt/DLSS.c`

**Current state:** DLSSApply() already has DLSSD eval params structure. Verify:

1. **Input color** should be noisy/raw (not denoised) when RR is active
2. **All gbuffer inputs** should point to populated DLSS_ buffers
3. **Hit distances** (diffuse/specular) should be valid ray lengths
4. **Motion vectors** should include reflection motion for proper temporal stability

**Key eval params for RR:**
```c
NVSDK_NGX_VK_DLSSD_Eval_Params evalParamsDlssd = {
    .pInColor = &unresolvedColorResource,        // Raw/noisy color
    .pInOutput = &resolvedColorResource,         // DLSS output
    .pInDepth = &depthResource,
    .pInMotionVectors = &motionVectorsResource,
    .pInRayTracingHitDistance = &rayLengthResource,
    .pInMotionVectors3D = &motionVec3D,
    .pInTransparencyMask = &transparentResoruce,
    .pInMotionVectorsReflections = &reflectMotion,
    .GBufferSurface = inBuffer,  // Contains albedo, specular, roughness, etc.
    // RR-specific:
    .pInDiffuseAlbedo = &indirectAlbedo,
    .pInSpecularAlbedo = &specularAlbedo,
    .pInNormals = &normal,
    .pInRoughness = &roughness,
    .pInDiffuseHitDistance = &diffuseLength,
    .pInSpecularHitDistance = &specularLength,
    .pInReflectedAlbedo = &reflectedAlbedo,
    .pInColorBeforeTransparency = &beforeTransparent,
};
```

---

## Data Flow Summary

```
[Path Tracer Shaders]
        |
        v
  [PT_ Buffers] (render resolution)
        |
        | copy/resample
        v
  [DLSS_ Buffers] (DLSS input resolution)
        |
        v
  [DLSS-RR Evaluate]
        |
        v
  [DLSS_OUTPUT] (native resolution)
        |
        v
  [Final Blit to Screen]
```

---

## Files to Modify

| File | Changes |
|------|---------|
| `src/refresh/vkpt/shader/global_textures.h` | Add PT_ image definitions |
| `src/refresh/vkpt/shader/primary_rays.rgen` | Write PT_ALBEDO, PT_ROUGHNESS, PT_METALLIC, PT_NORMAL |
| `src/refresh/vkpt/shader/direct_lighting.rgen` | Write PT_SPECULAR, PT_NORMAL, PT_SPECULAR_ALBEDO |
| `src/refresh/vkpt/shader/reflect_refract.rgen` | Write PT_REFLECTED_ALBEDO, PT_RAYLENGTH_SPECULAR |
| `src/refresh/vkpt/shader/indirect_lighting.rgen` | Write PT_RAYLENGTH_DIFFUSE |
| `src/refresh/vkpt/shader/compositing.comp` | Write PT_BEFORE_TRANSPARENT |
| `src/refresh/vkpt/main.c` | Clear PT_ buffers, copy to DLSS_ buffers |
| `src/refresh/vkpt/DLSS.c` | Verify eval params (may already be correct) |

---

## Reference Files

All reference implementations are in:
`Additional sources for help/Q2RTX-MOD-master/src/refresh/vkpt/`

Key files to compare:
- `shader/global_textures.h` - Image definitions
- `shader/primary_rays.rgen` - Primary surface data writes
- `shader/direct_lighting.rgen` - Lighting data writes
- `main.c` - Buffer clearing and copying logic
- `DLSS.c` - DLSS apply function

---

## Build Order

1. Add PT_ images to global_textures.h
2. Rebuild shaders (`cmake --build . --target shaders`)
3. Update shader files to write PT_ buffers
4. Rebuild shaders
5. Add clearing/copying in main.c
6. Full rebuild (`cmake --build . --config RelWithDebInfo`)
7. Test with `pt_dlss_dldn 1`

---

## Testing

1. Set `pt_dlss 3` (balanced preset) + `pt_dlss_dldn 1` (Ray Reconstruction)
2. Use `pt_dlss_debug N` to visualize individual buffers:
   - 4 = DLSS_ALBEDO
   - 6 = DLSS_ROUGHNESS
   - 8 = DLSS_NORMAL
   - 17 = DLSS_RAYLENGTH_DIFFUSE
   - 18 = DLSS_RAYLENGTH_SPECULAR
3. Verify each buffer contains expected data (not black)
4. Compare output quality vs DLSS-SR + ASVGF

# DLSS Ray Reconstruction Integration Plan for Q2RTX

## Overview
Port DLSS + Ray Reconstruction from Q2RTX-MOD reference implementation to current Q2RTX fork.

## Current Implementation Status

### ✅ COMPLETED
- [x] **DLSS.c and DLSS.h created** - Core NGX SDK integration
- [x] **CMakeLists.txt modified** - Added CONFIG_USE_DLSS option, include paths, library linking
- [x] **Base images added** - DLSS_OUTPUT (38), DLSS_DEPTH (39), PT_DLSS_MOTION (40), PT_REFLECT_MOTION (41)
- [x] **DLSS init/shutdown integrated** - DLSSConstructor/DLSSDeconstructor called in main.c
- [x] **Motion vector output** - asvgf_taau.comp writes PT_DLSS_MOTION
- [x] **Depth output** - checkerboard_interleave.comp writes DLSS_DEPTH
- [x] **DLSS apply hook** - DLSSApply called after vkpt_taa in render pipeline
- [x] **Final blit integration** - Uses DLSS_OUTPUT when DLSS enabled
- [x] **FSR disabled when DLSS active** - vkpt_fsr_is_enabled returns false when DLSS active
- [x] **Barriers added** - Proper synchronization for DLSS images

### 🔄 IN PROGRESS
- [ ] Testing and validation

### ⏳ PENDING (For Ray Reconstruction)
- [ ] Add Ray Reconstruction specific images (PT_ALBEDO, DLSS_ALBEDO, etc.)
- [ ] G-buffer output from path tracer
- [ ] DLSS-RR mode selection
- [ ] NRD replacement

---

## Source Reference
- **Location**: `Additional sources for help/Q2RTX-MOD-master/`
- **Key Files**: 
  - `src/refresh/vkpt/DLSS.c` (~823 lines)
  - `src/refresh/vkpt/DLSS.h` (~113 lines)
  - `src/refresh/vkpt/shader/global_textures.h` (38 new images)

## SDK Location
- **NGX SDK**: `extern/RTX-Kit/dlss/` (standalone)
- **Streamline**: `extern/RTX-Kit/streamline/` (alternative integration path)

---

## Phase 1: Foundation Setup ✅ COMPLETE

### 1.1 CMakeLists.txt Modifications
**File**: `src/CMakeLists.txt`

```cmake
# Add after existing options (around line ~19)
option(CONFIG_USE_DLSS "Enable NVIDIA DLSS support" ON)

# Add DLSS.c to SRC_VKPT list (around line ~263)
# Already present in reference: refresh/vkpt/DLSS.c

# Add DLSS linking (around line ~565)
if(CONFIG_USE_DLSS)
    message(STATUS "Q2RTX_WITH_NVIDIA_DLSS enabled")
    add_definitions(-DRG_USE_NVIDIA_DLSS)
    
    # Point to RTX-Kit DLSS SDK
    set(DLSS_SDK_PATH "${CMAKE_SOURCE_DIR}/extern/RTX-Kit/dlss")
    target_include_directories(client PRIVATE "${DLSS_SDK_PATH}/include")
    
    if(WIN32)
        add_definitions(-DNV_WINDOWS)
        target_link_libraries(client "${DLSS_SDK_PATH}/lib/Windows_x86_64/x86_64/nvsdk_ngx_s.lib")
    endif()
endif()
```

### 1.2 Copy Core DLSS Files
```
Source → Destination
─────────────────────
Additional sources for help/Q2RTX-MOD-master/src/refresh/vkpt/DLSS.c → src/refresh/vkpt/DLSS.c
Additional sources for help/Q2RTX-MOD-master/src/refresh/vkpt/DLSS.h → src/refresh/vkpt/DLSS.h
```

---

## Phase 2: Image/Buffer Infrastructure

### 2.1 New Images Required (global_textures.h)
Add these 38 new image definitions to `LIST_IMAGES`:

| Index | Name | Format | Size | Purpose |
|-------|------|--------|------|---------|
| 37 | PT_REFLECT_MOTION | R16G16B16A16_SFLOAT | Full | Reflection motion vectors |
| 38 | DLSS_OUTPUT | R16G16B16A16_SFLOAT | Unscaled | Final DLSS output |
| 39 | PT_VIEW_DEPTH_SPLIT | R32G32B32A32_SFLOAT | Full | Split depth buffer |
| 40 | PT_RAY_LENGTH | R32G32_SFLOAT | TAA | Ray hit distances |
| 41 | PT_DLSS_MOTION | R16G16B16A16_SFLOAT | TAA | DLSS-format motion vectors |
| 42 | DLSS_DEPTH | R32G32B32A32_SFLOAT | Unscaled | Linear depth for DLSS |
| 43 | DLSS_RAY_LENGTH | R32G32B32A32_SFLOAT | TAA | Ray length for RR |
| 44 | DLSS_TRANSPARENT | R32G32B32A32_SFLOAT | TAA | Transparency mask |
| 45 | DLSS_REFLECT_MOTION | R16G16B16A16_SFLOAT | TAA | Reflection motion |
| 46 | PT_ALBEDO | R16G16B16A16_SFLOAT | Full | Diffuse albedo |
| 47 | DLSS_ALBEDO | R16G16B16A16_SFLOAT | TAA | Albedo for RR |
| 48 | PT_SPECULAR | R16G16B16A16_SFLOAT | Full | Specular color |
| 49 | DLSS_SPECULAR | R16G16B16A16_SFLOAT | TAA | Specular for RR |
| 50 | PT_ROUGHNESS | R16G16B16A16_SFLOAT | Full | Surface roughness |
| 51 | DLSS_ROUGHNESS | R16G16B16A16_SFLOAT | TAA | Roughness for RR |
| 52 | PT_METALLIC | R16G16B16A16_SFLOAT | Full | Metallic factor |
| 53 | DLSS_METALLIC | R16G16B16A16_SFLOAT | TAA | Metallic for RR |
| 54 | PT_NORMAL | R16G16B16A16_SFLOAT | Full | World-space normals |
| 55 | DLSS_NORMAL | R16G16B16A16_SFLOAT | TAA | Normals for RR |
| 56 | PT_MATERIALID | R16G16B16A16_SFLOAT | Full | Material IDs |
| 57 | DLSS_MATERIALID | R16G16B16A16_SFLOAT | TAA | Material IDs for RR |
| 58 | PT_EMISSIVE | R16G16B16A16_SFLOAT | Full | Emissive contribution |
| 59 | DLSS_EMISSIVE | R16G16B16A16_SFLOAT | TAA | Emissive for RR |
| 60 | PT_INDIRECT_ALBEDO | R16G16B16A16_SFLOAT | Full | Indirect diffuse albedo |
| 61 | DLSS_INDIRECT_ALBEDO | R16G16B16A16_SFLOAT | TAA | Indirect albedo for RR |
| 62 | DLSS_3DMOTION_VECTOR | R16G16B16A16_SFLOAT | TAA | 3D motion vectors |
| 63 | PT_SPECULAR_ALBEDO | R16G16B16A16_SFLOAT | Full | Specular albedo |
| 64 | DLSS_SPECULAR_ALBEDO | R16G16B16A16_SFLOAT | TAA | Specular albedo for RR |
| 65 | PT_BEFORE_TRANSPARENT | R16G16B16A16_SFLOAT | Full | Color before transparency |
| 66 | DLSS_BEFORE_TRANSPARENT | R16G16B16A16_SFLOAT | TAA | Pre-transparent for RR |
| 67 | PT_RAYLENGTH_DIFFUSE | R16G16B16A16_SFLOAT | Full | Diffuse ray hit distance |
| 68 | DLSS_RAYLENGTH_DIFFUSE | R16G16B16A16_SFLOAT | TAA | Diffuse hit dist for RR |
| 69 | PT_RAYLENGTH_SPECULAR | R16G16B16A16_SFLOAT | Full | Specular ray hit distance |
| 70 | DLSS_RAYLENGTH_SPECULAR | R16G16B16A16_SFLOAT | TAA | Specular hit dist for RR |
| 71 | PT_REFLECTED_ALBEDO | R16G16B16A16_SFLOAT | Full | Reflection albedo |
| 72 | DLSS_REFLECTED_ALBEDO | R16G16B16A16_SFLOAT | Full | Reflection albedo for RR |
| 73 | DLSS_BLOOM_HBLUR | R16G16B16A16_SFLOAT | Unscaled/4 | Post-DLSS bloom H |
| 74 | DLSS_BLOOM_VBLUR | R16G16B16A16_SFLOAT | Unscaled/4 | Post-DLSS bloom V |
| 75 | PT_COMPOSITE_TRANSPARENT | R16G16B16A16_SFLOAT | Full | Composited transparency |

### 2.2 Update NUM_IMAGES_BASE
```c
#define NUM_IMAGES_BASE 76  // Was 38, now 76
```

---

## Phase 3: vkpt.h Modifications

### 3.1 Add extent_unscaled Support
Already exists, but verify:
```c
VkExtent2D extent_unscaled;  // Native resolution for DLSS output
```

### 3.2 Add Image Dimension Macros
```c
#define IMG_WIDTH_UNSCALED  (qvk.extent_unscaled.width)
#define IMG_HEIGHT_UNSCALED (qvk.extent_unscaled.height)
```

---

## Phase 4: Shader Modifications

### 4.1 Primary Rays (primary_rays.rgen)
Output additional G-buffer data:
- `IMG_PT_ALBEDO` - diffuse albedo
- `IMG_PT_NORMAL` - world normals (DLSS format)
- `IMG_PT_ROUGHNESS` - surface roughness
- `IMG_PT_METALLIC` - metallic factor
- `IMG_PT_MATERIALID` - material identification
- `IMG_PT_VIEW_DEPTH_SPLIT` - split linear depth

### 4.2 Direct/Indirect Lighting (direct_lighting.rgen, indirect_lighting.rgen)
Output:
- `IMG_PT_EMISSIVE` - emissive lighting
- `IMG_PT_SPECULAR` - specular lighting (before demodulation)
- `IMG_PT_RAYLENGTH_DIFFUSE` - diffuse ray hit distance
- `IMG_PT_RAYLENGTH_SPECULAR` - specular ray hit distance
- `IMG_PT_INDIRECT_ALBEDO` - GI albedo
- `IMG_PT_SPECULAR_ALBEDO` - specular albedo

### 4.3 Reflect/Refract (reflect_refract.rgen)
Output:
- `IMG_PT_REFLECT_MOTION` - reflection motion vectors
- `IMG_PT_REFLECTED_ALBEDO` - reflection albedo

### 4.4 ASVGF Temporal (asvgf_temporal.comp)
Add DLSS motion vector conversion:
- Convert Q2RTX motion format → DLSS motion format
- Output to `IMG_PT_DLSS_MOTION`

### 4.5 Compositing (compositing.comp)
Add DLSS buffer preparation:
- Interleave checkerboard data to DLSS buffers
- Handle `IMG_PT_BEFORE_TRANSPARENT`
- Convert depth to linear DLSS format

---

## Phase 5: Render Pipeline Integration

### 5.1 main.c Modifications

**Initialization** (in `R_Init_RTX`):
```c
#ifdef RG_USE_NVIDIA_DLSS
    InitDLSSCvars();
    DLSSConstructor(qvk.instance, qvk.device, qvk.physical_device, 
                    "Q2RTX-Fork-DLSS", qfalse);
#endif
```

**Shutdown** (in `R_Shutdown_RTX`):
```c
#ifdef RG_USE_NVIDIA_DLSS
    DLSSDeconstructor();
#endif
```

### 5.2 asvgf.c / tone_mapping.c Modifications

**In ASVGF pass** (after temporal accumulation):
- If DLSS enabled: prepare DLSS input buffers
- Copy/convert data to DLSS-sized buffers

**In Tone Mapping** (replace FSR/TAA output path):
```c
if (DLSSEnabled()) {
    DLSSApply(cmd_buf, qvk, resolution_info, jitter_offset, 
              frame_time, reset_accumulation);
    // Use DLSS_OUTPUT instead of TAA_OUTPUT
} else {
    // Existing FSR/TAA path
}
```

### 5.3 Image Layout Transitions
Add proper barriers for all new DLSS images before/after use.

---

## Phase 6: CVARs and Menu

### 6.1 New CVARs
```
pt_dlss          0|1|2|3|4|5  (off, perf, balanced, quality, ultra, DLAA)
pt_dlss_dldn     0|1          (Ray Reconstruction: off, on)
pt_dlss_debug    0-30         (Debug visualization mode)
```

### 6.2 Menu Integration
Add DLSS options to video settings menu.

---

## Phase 7: DLL Deployment

### 7.1 Required DLLs (from RTX-Kit)
Copy to executable directory:
```
extern/RTX-Kit/dlss/lib/Windows_x86_64/rel/nvngx_dlss.dll
```

For Ray Reconstruction (if using newer SDK):
```
nvngx_dlssd.dll  (DLSS-D / Ray Reconstruction)
```

---

## Implementation Order

1. **Day 1**: CMake setup + DLSS.c/DLSS.h copy + basic compilation
2. **Day 2**: Image infrastructure (global_textures.h, vkpt.h)
3. **Day 3**: Shader modifications (G-buffer outputs)
4. **Day 4**: Pipeline integration + testing

---

## Quick Start (Minimal DLSS Upscaling Only)

For basic DLSS without Ray Reconstruction:
1. Copy DLSS.c/DLSS.h
2. Add ~10 essential images (DLSS_OUTPUT, DLSS_DEPTH, PT_DLSS_MOTION)
3. Hook into tone_mapping.c after ASVGF
4. Disable DLSS-D mode (pt_dlss_dldn = 0)

This gives functional DLSS upscaling without the full G-buffer requirements.

---

## Files to Modify Summary

| File | Changes |
|------|---------|
| `src/CMakeLists.txt` | Add DLSS option, SDK linking |
| `src/refresh/vkpt/DLSS.c` | NEW - copy from reference |
| `src/refresh/vkpt/DLSS.h` | NEW - copy from reference |
| `src/refresh/vkpt/shader/global_textures.h` | Add 38 new images |
| `src/refresh/vkpt/vkpt.h` | Add DLSS-related declarations |
| `src/refresh/vkpt/main.c` | Init/shutdown DLSS |
| `src/refresh/vkpt/asvgf.c` | DLSS buffer preparation |
| `src/refresh/vkpt/tone_mapping.c` | DLSS apply call |
| `src/refresh/vkpt/shader/primary_rays.rgen` | G-buffer outputs |
| `src/refresh/vkpt/shader/direct_lighting.rgen` | G-buffer outputs |
| `src/refresh/vkpt/shader/indirect_lighting.rgen` | G-buffer outputs |
| `src/refresh/vkpt/shader/asvgf_temporal.comp` | Motion vector conversion |
| `src/refresh/vkpt/shader/compositing.comp` | DLSS buffer interleave |

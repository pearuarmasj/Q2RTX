#ifndef NRD_H_
#define NRD_H_

#include "vkpt.h"

#ifdef CONFIG_USE_NRD

// NRD configuration
#define NRD_MAX_PIPELINES 64
#define NRD_MAX_PERMANENT_TEXTURES 32
#define NRD_MAX_TRANSIENT_TEXTURES 32
#define NRD_QUEUED_FRAMES 2

// Initialize NRD subsystem
qboolean vkpt_nrd_init(void);

// Destroy NRD subsystem
void vkpt_nrd_destroy(void);

// Create NRD resources for given resolution
qboolean vkpt_nrd_create_resources(uint32_t width, uint32_t height);

// Destroy NRD resources (on resize or shutdown)
void vkpt_nrd_destroy_resources(void);

// Run NRD denoising pass
// Takes diffuse/specular radiance+hitdist as input, outputs denoised result
void vkpt_nrd_denoise(VkCommandBuffer cmd_buf, uint32_t frame_num);

// Check if NRD is enabled
qboolean vkpt_nrd_enabled(void);

// CVars
extern cvar_t* cvar_pt_nrd_enable;
extern cvar_t* cvar_pt_nrd_blur_radius;
extern cvar_t* cvar_pt_nrd_accumulation;

#endif // CONFIG_USE_NRD

#endif // NRD_H_

/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

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

#include "path_tracer.h"
#include "utils.glsl"
#include "path_tracer_transparency.glsl"

#define RAY_GEN_DESCRIPTOR_SET_IDX 0
layout(set = RAY_GEN_DESCRIPTOR_SET_IDX, binding = 0)
uniform accelerationStructureEXT topLevelAS[TLAS_COUNT];


#define GLOBAL_TEXTURES_DESC_SET_IDX 2
#include "global_textures.h"

#define VERTEX_BUFFER_DESC_SET_IDX 3
#define VERTEX_READONLY 1
#include "vertex_buffer.h"

#include "asvgf.glsl"
#include "brdf.glsl"
#include "water.glsl"
#include "tiny_encryption_algorithm.h"

/* RNG seeds contain 'X' and 'Y' values that are computed w/ a modulo BLUE_NOISE_RES,
 * so the shift values can be chosen to fit BLUE_NOISE_RES - 1
 * (see generate_rng_seed()) */
#define RNG_SEED_SHIFT_X        0u
#define RNG_SEED_SHIFT_Y        8u
#define RNG_SEED_SHIFT_ISODD    16u
#define RNG_SEED_SHIFT_FRAME    17u

#define RNG_PRIMARY_OFF_X   0
#define RNG_PRIMARY_OFF_Y   1
#define RNG_PRIMARY_APERTURE_X   2
#define RNG_PRIMARY_APERTURE_Y   3

#define RNG_NEE_LIGHT_SELECTION(bounce)   (4 + 0 + 9 * bounce)
#define RNG_NEE_TRI_X(bounce)             (4 + 1 + 9 * bounce)
#define RNG_NEE_TRI_Y(bounce)             (4 + 2 + 9 * bounce)
#define RNG_NEE_LIGHT_TYPE(bounce)        (4 + 3 + 9 * bounce)
#define RNG_BRDF_X(bounce)                (4 + 4 + 9 * bounce)
#define RNG_BRDF_Y(bounce)                (4 + 5 + 9 * bounce)
#define RNG_BRDF_FRESNEL(bounce)          (4 + 6 + 9 * bounce)
#define RNG_SUNLIGHT_X(bounce)			  (4 + 7 + 9 * bounce)
#define RNG_SUNLIGHT_Y(bounce)			  (4 + 8 + 9 * bounce)

#define PRIMARY_RAY_CULL_MASK        (AS_FLAG_OPAQUE | AS_FLAG_TRANSPARENT | AS_FLAG_VIEWER_WEAPON | AS_FLAG_SKY)
#define REFLECTION_RAY_CULL_MASK     (AS_FLAG_OPAQUE | AS_FLAG_SKY)
#define BOUNCE_RAY_CULL_MASK         (AS_FLAG_OPAQUE | AS_FLAG_SKY | AS_FLAG_CUSTOM_SKY)
#define SHADOW_RAY_CULL_MASK         (AS_FLAG_OPAQUE)

/* no BRDF sampling in last bounce */
#define NUM_RNG_PER_FRAME (RNG_NEE_STATIC_DYNAMIC(1) + 1)

#define BOUNCE_SPECULAR 1

#define MAX_OUTPUT_VALUE 1000

#ifdef KHR_RAY_QUERY

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Just global variables in RQ mode.
// No shadow payload necessary.
RayPayloadGeometry ray_payload_geometry;
RayPayloadEffects ray_payload_effects;

#include "path_tracer_hit_shaders.h"

#else // !KHR_RAY_QUERY

layout(location = RT_PAYLOAD_GEOMETRY) rayPayloadEXT RayPayloadGeometry ray_payload_geometry;
layout(location = RT_PAYLOAD_EFFECTS) rayPayloadEXT RayPayloadEffects ray_payload_effects;

#endif

uint rng_seed;

struct Ray {
	vec3 origin, direction;
	float t_min, t_max;
};

vec3
env_map(vec3 direction, bool remove_sun)
{
	direction = (global_ubo.environment_rotation_matrix * vec4(direction, 0)).xyz;

    vec3 envmap = vec3(0);
    if (global_ubo.environment_type == ENVIRONMENT_DYNAMIC)
    {
	    envmap = textureLod(TEX_PHYSICAL_SKY, direction.xzy, 0).rgb;

	    if(remove_sun)
	    {
			// roughly remove the sun from the env map
			envmap = min(envmap, vec3((1 - dot(direction, global_ubo.sun_direction_envmap)) * 200));
		}
	}
    else if (global_ubo.environment_type == ENVIRONMENT_STATIC)
    {
        envmap = textureLod(TEX_ENVMAP, direction.xzy, 0).rgb;
        float avg = (envmap.x + envmap.y + envmap.z) / 3.0;
        envmap = mix(envmap, avg.xxx, global_ubo.pt_envmap_desaturate) * global_ubo.pt_envmap_brightness;
    }
	return envmap;
}

// depends on env_map
#include "light_lists.h"

ivec2 get_image_position()
{
	ivec2 pos;

	bool is_even_checkerboard = push_constants.gpu_index == 0 || push_constants.gpu_index < 0 && rt_LaunchID.z == 0;
	if(global_ubo.pt_swap_checkerboard != 0)
		is_even_checkerboard = !is_even_checkerboard;

	if (is_even_checkerboard) {
		pos.x = int(rt_LaunchID.x * 2) + int(rt_LaunchID.y & 1);
	} else {
		pos.x = int(rt_LaunchID.x * 2 + 1) - int(rt_LaunchID.y & 1);
	}

	pos.y = int(rt_LaunchID.y);
	return pos;
}

ivec2 get_image_size()
{
	return ivec2(global_ubo.width, global_ubo.height);
}

bool
found_intersection(RayPayloadGeometry rp)
{
	return rp.primitive_id != ~0u;
}

Triangle
get_hit_triangle(RayPayloadGeometry rp)
{
	return load_and_transform_triangle(
		/* instance_idx = */ rp.buffer_and_instance_idx >> 16,
		/* buffer_idx = */ rp.buffer_and_instance_idx & 0xffff,
		rp.primitive_id);
}

vec3
get_hit_barycentric(RayPayloadGeometry rp)
{
	vec3 bary;
	bary.yz = rp.barycentric;
	bary.x  = 1.0 - bary.y - bary.z;
	return bary;
}

float
get_rng(uint idx)
{
	uvec3 p = uvec3(rng_seed >> RNG_SEED_SHIFT_X, rng_seed >> RNG_SEED_SHIFT_Y, rng_seed >> RNG_SEED_SHIFT_ISODD);
	p.z = (p.z >> 1) + (p.z & 1);
	p.z = (p.z + idx);
	p &= uvec3(BLUE_NOISE_RES - 1, BLUE_NOISE_RES - 1, NUM_BLUE_NOISE_TEX - 1);

	return min(texelFetch(TEX_BLUE_NOISE, ivec3(p), 0).r, 0.9999999999999);
	//return fract(vec2(get_rng_uint(idx)) / vec2(0xffffffffu));
}

bool
is_water(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_WATER;
}

bool
is_slime(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_SLIME;
}

bool
is_lava(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_LAVA;
}

bool
is_glass(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_GLASS;
}

bool
is_transparent(uint material)
{
	uint kind = material & MATERIAL_KIND_MASK;
	return kind == MATERIAL_KIND_TRANSPARENT || kind == MATERIAL_KIND_TRANSP_MODEL;
}

bool
is_chrome(uint material)
{
	uint kind = material & MATERIAL_KIND_MASK;
	return kind == MATERIAL_KIND_CHROME || kind == MATERIAL_KIND_CHROME_MODEL;
}

bool
is_sky(uint material)
{
	uint kind = material & MATERIAL_KIND_MASK;
	return kind == MATERIAL_KIND_SKY;
}

bool
is_screen(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_SCREEN;
}

bool
is_camera(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_CAMERA;
}

vec3
correct_emissive(uint material_id, vec3 emissive)
{
	return max(vec3(0), emissive.rgb + vec3(EMISSIVE_TRANSFORM_BIAS));
}

void
trace_geometry_ray(Ray ray, bool cull_back_faces, int instance_mask)
{
	uint rayFlags = 0;
	if (cull_back_faces)
		rayFlags |= gl_RayFlagsCullBackFacingTrianglesEXT;
	rayFlags |= gl_RayFlagsSkipProceduralPrimitives;

	ray_payload_geometry.barycentric = vec2(0);
	ray_payload_geometry.primitive_id = ~0u;
	ray_payload_geometry.buffer_and_instance_idx = 0;
	ray_payload_geometry.hit_distance = 0;

#ifdef KHR_RAY_QUERY

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, instance_mask, 
		ray.origin, ray.t_min, ray.direction, ray.t_max);

	// Start traversal: return false if traversal is complete
	while (rayQueryProceedEXT(rayQuery))
	{
		uint sbtOffset = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rayQuery, false);
		int primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false);
		int instanceID = rayQueryGetIntersectionInstanceIdEXT(rayQuery, false);
		int geometryIndex = rayQueryGetIntersectionGeometryIndexEXT(rayQuery, false);
		uint instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false);
		float hitT = rayQueryGetIntersectionTEXT(rayQuery, false);
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rayQuery, false);
		bool isProcedural = rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionAABBEXT;

		switch(sbtOffset)
		{
		case SBTO_MASKED:
			if (pt_logic_masked(primitiveID, instanceID, geometryIndex, instanceCustomIndex, bary))
				rayQueryConfirmIntersectionEXT(rayQuery);
			break;
		}
	}

	if (rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionTriangleEXT)
	{
		pt_logic_rchit(ray_payload_geometry, 
			rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true),
			rayQueryGetIntersectionInstanceIdEXT(rayQuery, true),
			rayQueryGetIntersectionGeometryIndexEXT(rayQuery, true),
			rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true),
			rayQueryGetIntersectionTEXT(rayQuery, true),
			rayQueryGetIntersectionBarycentricsEXT(rayQuery, true));
	}

#else

	traceRayEXT( topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, instance_mask,
			SBT_RCHIT_GEOMETRY /*sbtRecordOffset*/, 0 /*sbtRecordStride*/, SBT_RMISS_EMPTY /*missIndex*/,
			ray.origin, ray.t_min, ray.direction, ray.t_max, RT_PAYLOAD_GEOMETRY);

#endif
}

float vmin(vec3 v) { return min(v.x, min(v.y, v.z)); }
float vmax(vec3 v) { return max(v.x, max(v.y, v.z)); }

// Loops over the defined fog volumes and finds the two closest ones along the ray.
// They are stored in the order of min distance in rp.fog1 (closer) and rp.fog2 (further away).
// If the ray starts in a fog volume, that volume will be rp.fog1 with t_min = ray.t_min.
void find_fog_volumes(inout RayPayloadEffects rp, Ray ray)
{
	vec3 inv_dir = vec3(1.0) / ray.direction;
	for (int i = 0; i < MAX_FOG_VOLUMES; i++)
	{
		const ShaderFogVolume volume = global_ubo.fog_volumes[i];

		if (volume.is_active == 0)
			return;

		vec3 t1 = (volume.mins - ray.origin) * inv_dir;
		vec3 t2 = (volume.maxs - ray.origin) * inv_dir;
		float t_in = vmax(min(t1, t2));
		float t_out = vmin(max(t1, t2));
		t_in = max(t_in, ray.t_min);
		t_out = min(t_out, ray.t_max);

		if (t_out > t_in)
		{
			vec2 first_t_min_max = unpackHalf2x16(rp.fog1.w);
			vec2 second_t_min_max = unpackHalf2x16(rp.fog2.w);

			bool replaces_first = t_in < first_t_min_max.x || first_t_min_max.y == 0;
			bool replaces_second = t_in < second_t_min_max.x || second_t_min_max.y == 0;

			if (replaces_first || replaces_second)
			{
				uvec4 packed;
				packed.xy = packHalf4x16(vec4(volume.color * global_ubo.pt_fog_brightness, 0));
				packed.z = packHalf2x16(vec2(t_in, t_out));

				// Convert the volumetric density function into a 1D function along the ray
				float density_variable = dot(volume.density.xyz, ray.direction) * 0.5;
				float density_constant = dot(volume.density.xyz, ray.origin) + volume.density.w;
				// Scale the density stored here because typical values are very small, in fp16 denormal range
				packed.w = packHalf2x16(vec2(density_variable, density_constant) * 65536.0);

				if (replaces_first)
				{
					// Push fog1 to fog2, replace fog1 with the new volume
					rp.fog2 = rp.fog1;
					rp.fog1 = packed;
				}
				else // if (replaces_second) -- must be true
				{
					// Replace fog2 with the new volume
					rp.fog2 = packed;
				}
			}
		}
	}
}

vec4
trace_effects_ray(Ray ray, bool skip_procedural)
{
	uint rayFlags = 0;
	if (skip_procedural)
		rayFlags |= gl_RayFlagsSkipProceduralPrimitives;
	
	uint instance_mask = AS_FLAG_EFFECTS;

	ray_payload_effects.transparency = uvec2(0);
	ray_payload_effects.distances = 0;
	ray_payload_effects.fog1 = uvec4(0);
	ray_payload_effects.fog2 = uvec4(0);
#ifndef KHR_RAY_QUERY
	ray_payload_effects.rayTmax = ray.t_max;
#endif

	if (!skip_procedural)
		find_fog_volumes(ray_payload_effects, ray);

#ifdef KHR_RAY_QUERY

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, topLevelAS[TLAS_INDEX_EFFECTS], rayFlags, instance_mask, 
		ray.origin, ray.t_min, ray.direction, ray.t_max);

	// Start traversal: return false if traversal is complete
	while (rayQueryProceedEXT(rayQuery))
	{
		uint sbtOffset = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rayQuery, false);
		int primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false);
		int instanceID = rayQueryGetIntersectionInstanceIdEXT(rayQuery, false);
		uint instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false);
		float hitT = rayQueryGetIntersectionTEXT(rayQuery, false);
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rayQuery, false);
		bool isProcedural = rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionAABBEXT;

		vec4 transparent = vec4(0);

		if (isProcedural)
		{
			if (!skip_procedural) // this should be a compile-time constant
			{
				// We only have one type of procedural primitives: beams.
				
				// Run the intersection shader first...
				float tShapeHit;
				vec2 beam_fade_and_thickness;
				bool intersectsWithBeam = pt_logic_beam_intersection(primitiveID,
					ray.origin, ray.direction, ray.t_min, ray.t_max,
					beam_fade_and_thickness, tShapeHit);

				// Then the any-hit shader.
				if (intersectsWithBeam)
				{
					transparent = pt_logic_beam(primitiveID, beam_fade_and_thickness, tShapeHit, ray.t_max);
					hitT = tShapeHit;
				}
			}
		}
		else
		{
			switch(sbtOffset)
			{
			case SBTO_PARTICLE: // particles
				transparent = pt_logic_particle(primitiveID, bary);
				break;

			case SBTO_EXPLOSION: // explosions
				transparent = pt_logic_explosion(primitiveID, instanceID, instanceCustomIndex, ray.direction, bary);
				break;

			case SBTO_SPRITE: // sprites
				transparent = pt_logic_sprite(primitiveID, bary);
				break;
			}
		}

		if (transparent.a > 0)
		{
			update_payload_transparency(ray_payload_effects, transparent, hitT);
		}
	}

#else

	traceRayEXT( topLevelAS[TLAS_INDEX_EFFECTS], rayFlags, instance_mask,
			SBT_RCHIT_EFFECTS /*sbtRecordOffset*/, 0 /*sbtRecordStride*/, SBT_RMISS_EMPTY /*missIndex*/,
			ray.origin, ray.t_min, ray.direction, ray.t_max, RT_PAYLOAD_EFFECTS);

#endif

	if (skip_procedural)
		return get_payload_transparency(ray_payload_effects);

	return get_payload_transparency_with_fog(ray_payload_effects, ray.t_max);
}

Ray get_shadow_ray(vec3 p1, vec3 p2, float tmin)
{
	vec3 l = p2 - p1;
	float dist = length(l);
	l /= dist;

	Ray ray;
	ray.origin = p1 + l * tmin;
	ray.t_min = 0;
	ray.t_max = dist - tmin - 0.01;
	ray.direction = l;

	return ray;
}

float
trace_shadow_ray(Ray ray, int cull_mask)
{
	const uint rayFlags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipProceduralPrimitives;


#ifdef KHR_RAY_QUERY

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, cull_mask, 
		ray.origin, ray.t_min, ray.direction, ray.t_max);

	while (rayQueryProceedEXT(rayQuery))
	{
		uint sbtOffset = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rayQuery, false);
		int primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false);
		int instanceID = rayQueryGetIntersectionInstanceIdEXT(rayQuery, false);
		int geometryIndex = rayQueryGetIntersectionGeometryIndexEXT(rayQuery, false);
		uint instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false);
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rayQuery, false);
		bool isProcedural = rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionAABBEXT;

		if (!isProcedural && sbtOffset == SBTO_MASKED)
		{
			if (pt_logic_masked(primitiveID, instanceID, geometryIndex, instanceCustomIndex, bary))
				rayQueryConfirmIntersectionEXT(rayQuery);
		}
	}

	if(rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT)
		return 0.0f;
	else
		return 1.0f;

#else

	ray_payload_geometry.barycentric = vec2(0);
	ray_payload_geometry.primitive_id = ~0u;
	ray_payload_geometry.buffer_and_instance_idx = 0;
	ray_payload_geometry.hit_distance = -1;

	traceRayEXT( topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, cull_mask,
			SBT_RCHIT_GEOMETRY /*sbtRecordOffset*/, 0 /*sbtRecordStride*/, SBT_RMISS_EMPTY /*missIndex*/,
			ray.origin, ray.t_min, ray.direction, ray.t_max, RT_PAYLOAD_GEOMETRY);

	return found_intersection(ray_payload_geometry) ? 0.0 : 1.0;

#endif
}

vec3
trace_caustic_ray(Ray ray, int surface_medium)
{
	ray_payload_geometry.barycentric = vec2(0);
	ray_payload_geometry.primitive_id = ~0u;
	ray_payload_geometry.buffer_and_instance_idx = 0;
	ray_payload_geometry.hit_distance = -1;


	uint rayFlags = gl_RayFlagsCullBackFacingTrianglesEXT | gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipProceduralPrimitives;
	uint instance_mask = AS_FLAG_TRANSPARENT;
	
#ifdef KHR_RAY_QUERY

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, instance_mask, 
		ray.origin, ray.t_min, ray.direction, ray.t_max);
	
	rayQueryProceedEXT(rayQuery);

	if (rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionTriangleEXT)
	{
		pt_logic_rchit(ray_payload_geometry, 
			rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true),
			rayQueryGetIntersectionInstanceIdEXT(rayQuery, true),
			rayQueryGetIntersectionGeometryIndexEXT(rayQuery, true),
			rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true),
			rayQueryGetIntersectionTEXT(rayQuery, true),
			rayQueryGetIntersectionBarycentricsEXT(rayQuery, true));
	}

#else

	traceRayEXT(topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, instance_mask, SBT_RCHIT_GEOMETRY, 0, SBT_RMISS_EMPTY,
			ray.origin, ray.t_min, ray.direction, ray.t_max, RT_PAYLOAD_GEOMETRY);

#endif

	float extinction_distance = ray.t_max - ray.t_min;
	vec3 throughput = vec3(1);

	if(found_intersection(ray_payload_geometry))
	{
		Triangle triangle = get_hit_triangle(ray_payload_geometry);
		
		vec3 geo_normal = triangle.normals[0];
		bool is_vertical = abs(geo_normal.z) < 0.1;

		if((is_water(triangle.material_id) || is_slime(triangle.material_id)) && !is_vertical)
		{
			vec3 position = ray.origin + ray.direction * ray_payload_geometry.hit_distance;
			vec3 w = get_water_normal(triangle.material_id, geo_normal, triangle.tangents[0], position, true);

			float caustic = clamp((1 - pow(clamp(1 - length(w.xz), 0, 1), 2)) * 100, 0, 8);
			caustic = mix(1, caustic, clamp(ray_payload_geometry.hit_distance * 0.02, 0, 1));
			throughput = vec3(caustic);

			if(surface_medium != MEDIUM_NONE)
			{
				extinction_distance = ray_payload_geometry.hit_distance;
			}
			else
			{
				if(is_water(triangle.material_id))
					surface_medium = MEDIUM_WATER;
				else
					surface_medium = MEDIUM_SLIME;

				extinction_distance = max(0, ray.t_max - ray_payload_geometry.hit_distance);
			}
		}
		else if(is_glass(triangle.material_id) || is_water(triangle.material_id) && is_vertical)
		{
			vec3 bary = get_hit_barycentric(ray_payload_geometry);
			vec2 tex_coord = triangle.tex_coords * bary;

			MaterialInfo minfo = get_material_info(triangle.material_id);

	    	vec3 base_color = vec3(minfo.base_factor);
	    	if (minfo.base_texture > 0)
	    		base_color *= global_textureLod(minfo.base_texture, tex_coord, 2).rgb;
	    	base_color = clamp(base_color, vec3(0), vec3(1));

			throughput = base_color;
		}
		else
		{
			throughput = vec3(clamp(1.0 - triangle.alpha, 0.0, 1.0));
		}
	}

	//return vec3(caustic);
	return extinction(surface_medium, extinction_distance) * throughput;
}

vec3 rgbToNormal(vec3 rgb, out float len)
{
    vec3 n = vec3(rgb.xy * 2 - 1, rgb.z);

    len = length(n);
    return len > 0 ? n / len : vec3(0);
}


float
AdjustRoughnessToksvig(float roughness, float normalMapLen, float mip_level)
{
	float effect = global_ubo.pt_toksvig * clamp(mip_level, 0, 1);
    float shininess = RoughnessSquareToSpecPower(roughness) * effect; // not squaring the roughness here - looks better this way
    float ft = normalMapLen / mix(shininess, 1.0f, normalMapLen);
    ft = max(ft, 0.01f);
    return SpecPowerToRoughnessSquare(ft * shininess / effect);
}

float
get_specular_sampled_lighting_weight(float roughness, vec3 N, vec3 V, vec3 L, float pdfw)
{
    float ggxVndfPdf = ImportanceSampleGGX_VNDF_PDF(max(roughness, 0.01), N, V, L);
  
    // Balance heuristic assuming one sample from each strategy: light sampling and BRDF sampling
    return clamp(pdfw / (pdfw + ggxVndfPdf), 0, 1);
}

void
get_direct_illumination(
	vec3 position, 
	vec3 normal, 
	vec3 geo_normal, 
	uint cluster_idx, 
	uint material_id,
	int shadow_cull_mask, 
	vec3 view_direction, 
	vec3 albedo,
	vec3 base_reflectivity,
	float specular_factor,
	float roughness, 
	int surface_medium, 
	bool enable_caustics, 
	float direct_specular_weight, 
	bool enable_polygonal,
	bool enable_dynamic,
	bool is_gradient, 
	int bounce,
	out vec3 diffuse,
	out vec3 specular)
{
	diffuse = vec3(0);
	specular = vec3(0);

	vec3 pos_on_light_polygonal;
	vec3 pos_on_light_dynamic;

	vec3 contrib_polygonal = vec3(0);
	vec3 contrib_dynamic = vec3(0);

	float alpha = square(roughness);
	float phong_exp = RoughnessSquareToSpecPower(alpha);
	float phong_scale = min(100, 1 / (M_PI * square(alpha)));
	float phong_weight = clamp(specular_factor * luminance(base_reflectivity) / (luminance(base_reflectivity) + luminance(albedo)), 0, 0.9);

	int polygonal_light_index = -1;
	float polygonal_light_pdfw = 0;
	bool polygonal_light_is_sky = false;

	vec3 rng = vec3(
		get_rng(RNG_NEE_LIGHT_SELECTION(bounce)),
		get_rng(RNG_NEE_TRI_X(bounce)),
		get_rng(RNG_NEE_TRI_Y(bounce)));

	/* polygonal light illumination */
	if(enable_polygonal) 
	{
		sample_polygonal_lights(
			cluster_idx,
			position, 
			normal, 
			geo_normal, 
			view_direction, 
			phong_exp, 
			phong_scale,
			phong_weight, 
			is_gradient, 
			pos_on_light_polygonal, 
			contrib_polygonal,
			polygonal_light_index,
			polygonal_light_pdfw,
			polygonal_light_is_sky,
			rng);
	}

	bool is_polygonal = true;
	float vis = 1;

	/* dynamic light illumination */
	if(enable_dynamic)
	{
		// Limit the solid angle of sphere lights for indirect lighting 
		// in order to kill some fireflies in locations with many sphere lights.
		// Example: green wall-lamp corridor in the "train" map.
		float max_solid_angle = (bounce == 0) ? 2 * M_PI : 0.02;
	
		sample_dynamic_lights(
			position,
			normal,
			geo_normal,
			max_solid_angle,
			pos_on_light_dynamic,
			contrib_dynamic,
			rng);
	}

	float spec_polygonal = phong(normal, normalize(pos_on_light_polygonal - position), view_direction, phong_exp) * phong_scale;
	float spec_dynamic = phong(normal, normalize(pos_on_light_dynamic - position), view_direction, phong_exp) * phong_scale;

	float l_polygonal  = luminance(abs(contrib_polygonal)) * mix(1, spec_polygonal, phong_weight);
	float l_dynamic = luminance(abs(contrib_dynamic)) * mix(1, spec_dynamic, phong_weight);
	float l_sum = l_polygonal + l_dynamic;

	bool null_light = (l_sum == 0);

	float w = null_light ? 0.5 : l_polygonal / (l_polygonal + l_dynamic);

	float rng2 = get_rng(RNG_NEE_LIGHT_TYPE(bounce));
	is_polygonal = (rng2 < w);
	vis = is_polygonal ? (1 / w) : (1 / (1 - w));
	vec3 pos_on_light = null_light ? position : (is_polygonal ? pos_on_light_polygonal : pos_on_light_dynamic);
	vec3 contrib = is_polygonal ? contrib_polygonal : contrib_dynamic;

	Ray shadow_ray = get_shadow_ray(position - view_direction * 0.01, pos_on_light, 0);
	
	vis *= trace_shadow_ray(shadow_ray, null_light ? 0 : shadow_cull_mask);
#ifdef ENABLE_SHADOW_CAUSTICS
	if(enable_caustics)
	{
		contrib *= trace_caustic_ray(shadow_ray, surface_medium);
	}
#endif

	/* 
		Accumulate light shadowing statistics to guide importance sampling on the next frame.
		Inspired by paper called "Adaptive Shadow Testing for Ray Tracing" by G. Ward, EUROGRAPHICS 1994.

		The algorithm counts the shadowed and unshadowed rays towards each light, per cluster,
		per surface orientation in each cluster. Orientation helps improve accuracy in cases 
		when a single cluster has different parts which have the same light mostly shadowed and 
		mostly unshadowed.

		On the next frame, the light CDF is built using the counts from this frame, or the frame
		before that in case of gradient rays. See light_lists.h for more info.

		Only applies to polygonal polygon lights (i.e. no model or beam lights) because the spherical
		polygon lights do not have polygonal indices, and it would be difficult to map them 
		between frames.
	*/
	if(global_ubo.pt_light_stats != 0 
		&& is_polygonal 
		&& !null_light
		&& polygonal_light_index >= 0 
		&& polygonal_light_index < global_ubo.num_static_lights)
	{
		uint addr = get_light_stats_addr(cluster_idx, polygonal_light_index, get_primary_direction(normal));

		// Offset 0 is unshadowed rays,
		// Offset 1 is shadowed rays
		if(vis == 0) addr += 1;

		// Increment the ray counter
		atomicAdd(light_stats_bufers[global_ubo.current_frame_idx % NUM_LIGHT_STATS_BUFFERS].stats[addr], 1);
	}

	if(null_light)
		return;

	vec3 radiance = vis * contrib;

	vec3 L = pos_on_light - position;
	L = normalize(L);

	if(is_polygonal && direct_specular_weight > 0 && polygonal_light_is_sky && global_ubo.pt_specular_mis != 0)
	{
		// MIS with direct specular and indirect specular.
		// Only applied to sky lights, for two reasons:
		//  1) Non-sky lights are trimmed to match the light texture, and indirect rays don't see that;
		//  2) Non-sky lights are usually away from walls, so the direct sampling issue is not as pronounced.

		direct_specular_weight *= get_specular_sampled_lighting_weight(roughness,
			normal, -view_direction, L, polygonal_light_pdfw);
	}

	vec3 F = vec3(0);

	if(vis > 0 && direct_specular_weight > 0)
	{
		vec3 specular_brdf = GGX_times_NdotL(view_direction, normalize(pos_on_light - position),
			normal, roughness, base_reflectivity, 0.0, specular_factor, F);
		specular = radiance * specular_brdf * direct_specular_weight;
	}

	float NdotL = max(0, dot(normal, L));

	float diffuse_brdf = NdotL / M_PI;
	diffuse = radiance * diffuse_brdf * (vec3(1.0) - F);
}

// =====================================================================================
// ReSTIR DI (spatiotemporal reservoir sampling)
//
// This implementation generates N unshadowed candidates at the current shading point,
// merges reprojected previous-frame reservoirs (temporal) and previous-frame neighbors
// (spatial) using stored reservoir state (W, M), and evaluates visibility (and optional
// caustics) only once for the final selected sample.
//
// Notes:
//   - Candidates are generated without per-candidate shadow rays; visibility is evaluated only
//     once for the final selected sample.
//   - Reservoir history is stored in ping-pong images:
//       * PT_RESTIR_DI_ID_[A|B]  : packed light kind/id
//       * PT_RESTIR_DI_RES_[A|B] : packed (W, M, y_pos)
// =====================================================================================

// These indices are only used to decorrelate candidate generation.
// get_rng() internally wraps indices, so the absolute values do not matter.
#define RNG_RESTIR_DI_BASE        997u
#define RNG_RESTIR_DI_STRIDE      5u
#define RNG_RESTIR_DI_LIGHT_SEL(i)  (RNG_RESTIR_DI_BASE + RNG_RESTIR_DI_STRIDE * uint(i) + 0u)
#define RNG_RESTIR_DI_TRI_X(i)      (RNG_RESTIR_DI_BASE + RNG_RESTIR_DI_STRIDE * uint(i) + 1u)
#define RNG_RESTIR_DI_TRI_Y(i)      (RNG_RESTIR_DI_BASE + RNG_RESTIR_DI_STRIDE * uint(i) + 2u)
#define RNG_RESTIR_DI_LIGHT_TYPE(i) (RNG_RESTIR_DI_BASE + RNG_RESTIR_DI_STRIDE * uint(i) + 3u)
#define RNG_RESTIR_DI_RESAMPLE(i)   (RNG_RESTIR_DI_BASE + RNG_RESTIR_DI_STRIDE * uint(i) + 4u)

// Debug kind codes:
//   0 = none / null
//   1 = polygonal light
//   2 = dynamic light
void
get_direct_illumination_sampled(
	vec3 position,
	vec3 normal,
	vec3 geo_normal,
	uint cluster_idx,
	uint material_id,
	int shadow_cull_mask,
	vec3 view_direction,
	vec3 albedo,
	vec3 base_reflectivity,
	float specular_factor,
	float roughness,
	int surface_medium,
	bool enable_caustics,
	float direct_specular_weight,
	bool enable_polygonal,
	bool enable_dynamic,
	bool is_gradient,
	int bounce,
	vec3 rng_light,
	float rng_light_type,
	out vec3 diffuse,
	out vec3 specular,
	out int debug_kind,
	out int debug_light_id,
	out float debug_visibility,
	out vec3 out_sample_pos_on_light,
	out vec3 out_sample_contrib,
	out float out_sample_vis_scale)
{
	diffuse = vec3(0);
	specular = vec3(0);
	debug_kind = 0;
	debug_light_id = -1;
	debug_visibility = 0;
	out_sample_pos_on_light = vec3(0);
	out_sample_contrib = vec3(0);
	out_sample_vis_scale = 0;

	vec3 pos_on_light_polygonal = vec3(0);
	vec3 pos_on_light_dynamic = vec3(0);

	vec3 contrib_polygonal = vec3(0);
	vec3 contrib_dynamic = vec3(0);

	float alpha = square(roughness);
	float phong_exp = RoughnessSquareToSpecPower(alpha);
	float phong_scale = min(100, 1 / (M_PI * square(alpha)));
	float phong_weight = clamp(specular_factor * luminance(base_reflectivity) / (luminance(base_reflectivity) + luminance(albedo)), 0, 0.9);

	int polygonal_light_index = -1;
	float polygonal_light_pdfw = 0;
	bool polygonal_light_is_sky = false;

	// Polygonal light illumination
	if(enable_polygonal)
	{
		sample_polygonal_lights(
			cluster_idx,
			position,
			normal,
			geo_normal,
			view_direction,
			phong_exp,
			phong_scale,
			phong_weight,
			is_gradient,
			pos_on_light_polygonal,
			contrib_polygonal,
			polygonal_light_index,
			polygonal_light_pdfw,
			polygonal_light_is_sky,
			rng_light);
	}

	// Dynamic light illumination
	uint dyn_light_idx = 0;
	if(enable_dynamic)
	{
		// Keep the same limiter as the baseline.
		float max_solid_angle = (bounce == 0) ? 2 * M_PI : 0.02;

		// Mirror the selection logic from sample_dynamic_lights for debug IDs.
		if(global_ubo.num_dyn_lights > 0)
		{
			float random_light = rng_light.x * float(global_ubo.num_dyn_lights);
			dyn_light_idx = min(uint(global_ubo.num_dyn_lights - 1), uint(random_light));
		}

		sample_dynamic_lights(
			position,
			normal,
			geo_normal,
			max_solid_angle,
			pos_on_light_dynamic,
			contrib_dynamic,
			rng_light);
	}

	float spec_polygonal = phong(normal, normalize(pos_on_light_polygonal - position), view_direction, phong_exp) * phong_scale;
	float spec_dynamic = phong(normal, normalize(pos_on_light_dynamic - position), view_direction, phong_exp) * phong_scale;

	float l_polygonal = luminance(abs(contrib_polygonal)) * mix(1, spec_polygonal, phong_weight);
	float l_dynamic = luminance(abs(contrib_dynamic)) * mix(1, spec_dynamic, phong_weight);
	float l_sum = l_polygonal + l_dynamic;

	bool null_light = (l_sum == 0);
	if(null_light)
		return;

	float w = l_polygonal / l_sum;
	bool is_polygonal = (rng_light_type < w);

	// Mixture compensation.
	float vis_scale = is_polygonal ? (1.0 / max(w, 1e-6)) : (1.0 / max(1.0 - w, 1e-6));
	vec3 pos_on_light = is_polygonal ? pos_on_light_polygonal : pos_on_light_dynamic;
	vec3 contrib = is_polygonal ? contrib_polygonal : contrib_dynamic;

	out_sample_pos_on_light = pos_on_light;
	out_sample_contrib = contrib;
	out_sample_vis_scale = vis_scale;

	// Unshadowed evaluation: assume visibility is 1 and apply mixture compensation.
	// Actual visibility (and optional caustics) are applied only once for the final selected sample.
	float vis = vis_scale;
	debug_visibility = vis;

	vec3 radiance = vis * contrib;

	vec3 L = normalize(pos_on_light - position);

	if(is_polygonal && direct_specular_weight > 0 && polygonal_light_is_sky && global_ubo.pt_specular_mis != 0)
	{
		direct_specular_weight *= get_specular_sampled_lighting_weight(
			roughness,
			normal,
			-view_direction,
			L,
			polygonal_light_pdfw);
	}

	vec3 F = vec3(0);
	if(vis > 0 && direct_specular_weight > 0)
	{
		vec3 specular_brdf = GGX_times_NdotL(
			view_direction,
			L,
			normal,
			roughness,
			base_reflectivity,
			0.0,
			specular_factor,
			F);
		specular = radiance * specular_brdf * direct_specular_weight;
	}

	float NdotL = max(0, dot(normal, L));
	float diffuse_brdf = NdotL / M_PI;
	diffuse = radiance * diffuse_brdf * (vec3(1.0) - F);

	if(is_polygonal)
	{
		debug_kind = 1;
		debug_light_id = polygonal_light_index;
	}
	else
	{
		debug_kind = 2;
		debug_light_id = int(dyn_light_idx);
	}
}

// Convert a checkerboarded pixel position (left and right fields) to flat-screen position.
// Matches the implementation in asvgf_temporal.comp.
ivec2 restir_di_checker_to_flat(ivec2 pos, int width)
{
	uint half_width = uint(width / 2);
	bool is_even_checkerboard = uint(pos.x) < half_width;

	return ivec2(
		is_even_checkerboard
			? (pos.x * 2) + (pos.y & 1)
			: ((pos.x - int(half_width)) * 2) + ((pos.y & 1) ^ 1),
		pos.y);
}

// Convert a flat-screen (regular) pixel position to checkerboarded (left and right fields).
// Matches the implementation in asvgf_temporal.comp.
ivec2 restir_di_flat_to_checker(ivec2 pos, int width)
{
	uint half_width = uint(width / 2);
	bool is_even_checkerboard = (pos.x & 1) == (pos.y & 1);
	return ivec2((pos.x / 2) + (is_even_checkerboard ? 0 : int(half_width)), pos.y);
}

bool restir_di_reproject_prev_pixel(ivec2 ipos, vec4 motion, bool sample_across_fields, out ivec2 ipos_prev)
{
	// For ReSTIR DI reuse, keep reprojection within the same checkerboard field.
	// This matches the reference ReSTIR implementation and avoids mixing histories
	// across fields (a common source of severe ghosting/artifacts).
	sample_across_fields = false;

	vec2 pos_prev;
	if(sample_across_fields)
	{
		// Reprojection in flat-screen coordinates.
		pos_prev = ((vec2(restir_di_checker_to_flat(ipos, global_ubo.width)) + vec2(0.5))
			* vec2(global_ubo.inv_width, global_ubo.inv_height) + motion.xy)
			* vec2(global_ubo.prev_width, global_ubo.prev_height);

		ivec2 p_flat = ivec2(floor(pos_prev - vec2(0.5)));
		if(p_flat.x < 0 || p_flat.y < 0 || p_flat.x >= global_ubo.prev_width || p_flat.y >= global_ubo.prev_height)
			return false;
		ipos_prev = restir_di_flat_to_checker(p_flat, global_ubo.prev_width);
		// Ensure we remain in the same field.
		int field_left = (ipos.x >= global_ubo.width / 2) ? (global_ubo.prev_width / 2) : 0;
		int field_right = field_left + (global_ubo.prev_width / 2);
		if(ipos_prev.x < field_left || ipos_prev.x >= field_right)
			return false;
		return true;
	}

	// Reprojection in checkerboarded coordinates.
	pos_prev = ((vec2(ipos) + vec2(0.5)) * vec2(global_ubo.inv_width * 2.0, global_ubo.inv_height) + motion.xy)
		* vec2(global_ubo.prev_width / 2.0, global_ubo.prev_height);
	ivec2 p = ivec2(floor(pos_prev - vec2(0.5)));
	if(p.x < 0 || p.y < 0 || p.x >= global_ubo.prev_width || p.y >= global_ubo.prev_height)
		return false;
	ipos_prev = p;
	// Ensure we remain in the same field.
	int field_left = (ipos.x >= global_ubo.width / 2) ? (global_ubo.prev_width / 2) : 0;
	int field_right = field_left + (global_ubo.prev_width / 2);
	if(ipos_prev.x < field_left || ipos_prev.x >= field_right)
		return false;
	return true;
}

// Best-effort light id remapping/validation for reuse:
// our reservoir stores a polygon light index; if that index is no longer present
// in the current cluster's light list, reject the reused reservoir.
bool restir_di_cluster_contains_light(uint list_idx, uint light_id)
{
	if(list_idx == ~0u)
		return false;

	uint list_start = light_buffer.light_list_offsets[list_idx];
	uint list_end = light_buffer.light_list_offsets[list_idx + 1];
	for(uint i = list_start; i < list_end; i++)
	{
		uint li = light_buffer.light_list_lights[i];
		if(li == light_id)
			return true;
	}
	return false;
}

// Packed reservoir storage.
// ID image stores (kind | (light_id << 2)). Kind uses the low 2 bits.
#define RESTIR_DI_KIND_NONE      0u
#define RESTIR_DI_KIND_POLYGONAL 1u
#define RESTIR_DI_KIND_SUN       2u

uint restir_di_pack_id(uint kind, uint light_id)
{
	return (kind & 0x3u) | (light_id << 2);
}

void restir_di_unpack_id(uint packed, out uint kind, out uint light_id)
{
	kind = packed & 0x3u;
	light_id = packed >> 2;
}

uvec2 restir_di_pack_reservoir(float W, int M, vec2 y_pos)
{
	uint packed_w_m = packHalf2x16(vec2(W, float(M)));
	uint packed_y = packHalf2x16(y_pos);
	return uvec2(packed_w_m, packed_y);
}

void restir_di_unpack_reservoir(uvec2 packed, out float W, out int M, out vec2 y_pos)
{
	vec2 wm = unpackHalf2x16(packed.x);
	W = wm.x;
	M = int(wm.y + 0.5);
	y_pos = unpackHalf2x16(packed.y);
}

bool restir_di_load_prev(
	ivec2 ipos_prev,
	uint list_idx,
	out uint kind,
	out uint light_id,
	out float reservoir_W,
	out int reservoir_M,
	out vec2 reservoir_y_pos)
{
	uint packed_id = texelFetch(TEX_PT_RESTIR_DI_ID_B, ipos_prev, 0).r;
	uvec2 packed_res = texelFetch(TEX_PT_RESTIR_DI_RES_B, ipos_prev, 0).xy;

	restir_di_unpack_id(packed_id, kind, light_id);
	restir_di_unpack_reservoir(packed_res, reservoir_W, reservoir_M, reservoir_y_pos);

	if(kind != RESTIR_DI_KIND_POLYGONAL && kind != RESTIR_DI_KIND_SUN)
		return false;
	if(kind == RESTIR_DI_KIND_POLYGONAL && light_id >= MAX_LIGHT_POLYS)
		return false;
	// Only allow reuse of static polygonal lights. Model/instanced lights can be re-ordered
	// between frames, which makes the stored light_id refer to a different emitter and causes
	// severe spatiotemporal artifacts.
	if(kind == RESTIR_DI_KIND_POLYGONAL && light_id >= uint(global_ubo.num_static_lights))
		return false;
	if(kind == RESTIR_DI_KIND_POLYGONAL && !restir_di_cluster_contains_light(list_idx, light_id))
		return false;
	if(!(reservoir_W > 0.0) || reservoir_M <= 0)
		return false;
	if(any(isnan(vec3(reservoir_W, reservoir_y_pos))) || any(isinf(vec3(reservoir_W, reservoir_y_pos))))
		return false;
	if(any(lessThan(reservoir_y_pos, vec2(0.0))) || any(greaterThanEqual(reservoir_y_pos, vec2(1.0))))
		return false;

	// CRITICAL: Clamp loaded W to prevent extreme values from corrupting combination.
	// Neighbors in dark areas can have huge W values (due to tiny p_hat), and when
	// these are combined with multiple spatial neighbors, the weights explode.
	float max_w = global_ubo.pt_restir_max_w;
	if(max_w > 0.0)
		reservoir_W = min(reservoir_W, max_w);

	return true;
}

bool restir_di_pick_polygonal_light_uniform(uint list_idx, float r, out uint light_id)
{
	if(list_idx == ~0u)
		return false;

	uint list_start = light_buffer.light_list_offsets[list_idx];
	uint list_end = light_buffer.light_list_offsets[list_idx + 1];
	uint light_count = list_end - list_start;
	if(light_count == 0)
		return false;

	uint base = min(light_count - 1u, uint(r * float(light_count)));
	for(uint t = 0u; t < 4u; t++)
	{
		uint idx = (base + t) % light_count;
		uint li = light_buffer.light_list_lights[list_start + idx];
		if(li < MAX_LIGHT_POLYS)
		{
			light_id = li;
			return true;
		}
	}

	return false;
}

bool restir_di_eval_polygonal_sample(
	uint list_idx,
	vec3 position,
	vec3 geo_normal,
	uint light_id,
	vec2 y_pos,
	out vec3 out_pos_on_light,
	out vec3 out_contrib,
	out float out_pdfw,
	out bool out_is_sky_light)
{
	out_pos_on_light = vec3(0);
	out_contrib = vec3(0);
	out_pdfw = 0.0;
	out_is_sky_light = false;

	if(list_idx == ~0u)
		return false;
	if(light_id >= MAX_LIGHT_POLYS)
		return false;

	uint list_start = light_buffer.light_list_offsets[list_idx];
	uint list_end = light_buffer.light_list_offsets[list_idx + 1];
	uint light_count = list_end - list_start;
	if(light_count == 0)
		return false;

	// Additional validation: ensure light_id is actually in this cluster's list.
	// This prevents using stale light IDs from temporal/spatial reuse that may
	// reference lights not visible from this shading point.
	if(!restir_di_cluster_contains_light(list_idx, light_id))
		return false;

	LightPolygon light = get_light_polygon(light_id);

	vec3 light_normal;
	out_pos_on_light = sample_projected_triangle(position, light.positions, y_pos, light_normal, out_pdfw);

	vec3 L = normalize(out_pos_on_light - position);
	if(dot(L, geo_normal) <= 0.0)
		out_pdfw = 0.0;

	if(out_pdfw <= 0.0)
		return false;

	float inv_pdfw = 1.0 / out_pdfw;
	float LdotNL = max(0.0, -dot(light_normal, L));
	float spotlight = sqrt(LdotNL);

	if(light.color.r >= 0.0)
	{
		out_contrib = light.color * (inv_pdfw * spotlight * light.light_style_scale);
	}
	else
	{
		out_contrib = env_map(L, true) * inv_pdfw * global_ubo.pt_env_scale;
		out_is_sky_light = true;
	}

	// Uniform selection among this cluster's polygonal light list.
	out_contrib *= float(light_count);

	// Sanity check: reject samples with NaN/Inf or excessively large values.
	if(any(isnan(out_contrib)) || any(isinf(out_contrib)) || luminance(out_contrib) > 1e6)
		return false;

	return true;
}

bool restir_di_evaluate_polygonal_unshadowed(
	uint list_idx,
	vec3 position,
	vec3 normal,
	vec3 geo_normal,
	vec3 view_direction,
	vec3 base_reflectivity,
	float specular_factor,
	float roughness,
	float direct_specular_weight,
	uint light_id,
	vec2 y_pos,
	out float out_p_hat)
{
	out_p_hat = 0.0;

	vec3 pos_on_light;
	vec3 contrib;
	float pdfw;
	bool is_sky_light;
	if(!restir_di_eval_polygonal_sample(list_idx, position, geo_normal, light_id, y_pos, pos_on_light, contrib, pdfw, is_sky_light))
		return false;

	vec3 L = normalize(pos_on_light - position);
	float NdotL = max(0.0, dot(normal, L));
	if(NdotL <= 0.0)
		return false;

	float spec_w = direct_specular_weight;
	if(spec_w > 0.0 && is_sky_light && global_ubo.pt_specular_mis != 0)
	{
		spec_w *= get_specular_sampled_lighting_weight(roughness, normal, -view_direction, L, pdfw);
	}

	vec3 F = vec3(0);
	vec3 specular = vec3(0);
	if(spec_w > 0.0)
	{
		vec3 specular_brdf = GGX_times_NdotL(view_direction, L,
			normal, roughness, base_reflectivity, 0.0, specular_factor, F);
		specular = contrib * specular_brdf * spec_w;
	}

	float diffuse_brdf = NdotL / M_PI;
	vec3 diffuse = contrib * diffuse_brdf * (vec3(1.0) - F);

	out_p_hat = max(luminance(abs(diffuse + specular)), 0.0);
	return out_p_hat > 0.0;
}

void restir_di_shade_polygonal_sample(
	uint list_idx,
	vec3 position,
	vec3 normal,
	vec3 geo_normal,
	vec3 view_direction,
	int shadow_cull_mask,
	vec3 base_reflectivity,
	float specular_factor,
	float roughness,
	float direct_specular_weight,
	int surface_medium,
	bool enable_caustics,
	uint light_id,
	vec2 y_pos,
	out vec3 diffuse,
	out vec3 specular,
	out float out_visibility)
{
	diffuse = vec3(0);
	specular = vec3(0);
	out_visibility = 0.0;

	vec3 pos_on_light;
	vec3 contrib;
	float pdfw;
	bool is_sky_light;
	if(!restir_di_eval_polygonal_sample(list_idx, position, geo_normal, light_id, y_pos, pos_on_light, contrib, pdfw, is_sky_light))
		return;

	Ray shadow_ray = get_shadow_ray(position - view_direction * 0.01, pos_on_light, 0);
	float vis = trace_shadow_ray(shadow_ray, shadow_cull_mask);
	out_visibility = vis;
	if(vis <= 0.0)
		return;

#ifdef ENABLE_SHADOW_CAUSTICS
	if(enable_caustics)
	{
		contrib *= trace_caustic_ray(shadow_ray, surface_medium);
	}
#endif

	vec3 radiance = vis * contrib;
	vec3 L = normalize(pos_on_light - position);

	float spec_w = direct_specular_weight;
	if(spec_w > 0.0 && is_sky_light && global_ubo.pt_specular_mis != 0)
	{
		spec_w *= get_specular_sampled_lighting_weight(roughness, normal, -view_direction, L, pdfw);
	}

	vec3 F = vec3(0);
	if(spec_w > 0.0)
	{
		vec3 specular_brdf = GGX_times_NdotL(view_direction, L,
			normal, roughness, base_reflectivity, 0.0, specular_factor, F);
		specular = radiance * specular_brdf * spec_w;
	}

	float NdotL = max(0.0, dot(normal, L));
	float diffuse_brdf = NdotL / M_PI;
	diffuse = radiance * diffuse_brdf * (vec3(1.0) - F);

	// Final sanity check: clamp extreme values and reject NaN/Inf.
	diffuse = clamp(diffuse, vec3(0), vec3(MAX_OUTPUT_VALUE));
	specular = clamp(specular, vec3(0), vec3(MAX_OUTPUT_VALUE));
	if(any(isnan(diffuse)) || any(isinf(diffuse)))
		diffuse = vec3(0);
	if(any(isnan(specular)) || any(isinf(specular)))
		specular = vec3(0);
}

bool restir_di_evaluate_sun_unshadowed(
	uint cluster_idx,
	vec3 position,
	vec3 normal,
	vec3 geo_normal,
	vec3 view_direction,
	vec3 base_reflectivity,
	float specular_factor,
	float roughness,
	vec2 y_pos,
	out float out_p_hat)
{
	out_p_hat = 0.0;

	if(global_ubo.sun_visible == 0)
		return false;

	bool visible = (cluster_idx == ~0u) || (light_buffer.sky_visibility[cluster_idx >> 5] & (1 << (cluster_idx & 31))) != 0;
	if(!visible)
		return false;

	vec2 disk = sample_disk(y_pos);
	disk.xy *= global_ubo.sun_tan_half_angle;

	vec3 direction = normalize(global_ubo.sun_direction + global_ubo.sun_tangent * disk.x + global_ubo.sun_bitangent * disk.y);

	float NdotL = dot(direction, normal);
	float GNdotL = dot(direction, geo_normal);
	if(NdotL <= 0 || GNdotL <= 0)
		return false;

#ifdef ENABLE_SUN_SHAPE
	vec3 envmap_direction = (global_ubo.environment_rotation_matrix * vec4(direction, 0)).xyz;
	vec3 envmap = textureLod(TEX_PHYSICAL_SKY, envmap_direction.xzy, 0).rgb;
	vec3 radiance = (global_ubo.sun_solid_angle * global_ubo.pt_env_scale) * envmap;
#else
	vec3 radiance = sun_color_ubo.sun_color;
#endif

	vec3 F = vec3(0);
	vec3 specular = vec3(0);
	if(global_ubo.pt_sun_specular > 0)
	{
		float NoH_offset = 0.5 * square(global_ubo.sun_tan_half_angle);
		vec3 specular_brdf = GGX_times_NdotL(view_direction, global_ubo.sun_direction,
			normal, roughness, base_reflectivity, NoH_offset, specular_factor, F);
		specular = radiance * specular_brdf;
	}

	float diffuse_brdf = NdotL / M_PI;
	vec3 diffuse = radiance * diffuse_brdf * (vec3(1.0) - F);

	out_p_hat = max(luminance(abs(diffuse + specular)), 0.0);
	return out_p_hat > 0.0;
}

void restir_di_shade_sun_sample(
	uint cluster_idx,
	vec3 position,
	vec3 normal,
	vec3 geo_normal,
	vec3 view_direction,
	int shadow_cull_mask,
	vec3 base_reflectivity,
	float specular_factor,
	float roughness,
	int surface_medium,
	bool enable_caustics,
	vec2 y_pos,
	out vec3 diffuse,
	out vec3 specular,
	out float out_visibility)
{
	diffuse = vec3(0);
	specular = vec3(0);
	out_visibility = 0.0;

	if(global_ubo.sun_visible == 0)
		return;

	bool visible = (cluster_idx == ~0u) || (light_buffer.sky_visibility[cluster_idx >> 5] & (1 << (cluster_idx & 31))) != 0;
	if(!visible)
		return;

	vec2 disk = sample_disk(y_pos);
	disk.xy *= global_ubo.sun_tan_half_angle;

	vec3 direction = normalize(global_ubo.sun_direction + global_ubo.sun_tangent * disk.x + global_ubo.sun_bitangent * disk.y);

	float NdotL = dot(direction, normal);
	float GNdotL = dot(direction, geo_normal);
	if(NdotL <= 0 || GNdotL <= 0)
		return;

	Ray shadow_ray = get_shadow_ray(position - view_direction * 0.01, position + direction * 10000, 0);
	float vis = trace_shadow_ray(shadow_ray, shadow_cull_mask);
	out_visibility = vis;
	if(vis <= 0.0)
		return;

#ifdef ENABLE_SUN_SHAPE
	vec3 envmap_direction = (global_ubo.environment_rotation_matrix * vec4(direction, 0)).xyz;
	vec3 envmap = textureLod(TEX_PHYSICAL_SKY, envmap_direction.xzy, 0).rgb;
	vec3 radiance = (global_ubo.sun_solid_angle * global_ubo.pt_env_scale) * envmap;
#else
	vec3 radiance = sun_color_ubo.sun_color;
#endif

#ifdef ENABLE_SHADOW_CAUSTICS
	if(enable_caustics)
	{
		radiance *= trace_caustic_ray(shadow_ray, surface_medium);
	}
#endif

	vec3 F = vec3(0);
	if(global_ubo.pt_sun_specular > 0)
	{
		float NoH_offset = 0.5 * square(global_ubo.sun_tan_half_angle);
		vec3 specular_brdf = GGX_times_NdotL(view_direction, global_ubo.sun_direction,
			normal, roughness, base_reflectivity, NoH_offset, specular_factor, F);
		specular = radiance * specular_brdf;
	}

	float diffuse_brdf = NdotL / M_PI;
	diffuse = radiance * diffuse_brdf * (vec3(1.0) - F);
}

void
get_direct_illumination_restir_di(
	ivec2 ipos,
	int checkerboard_flags,
	vec3 position,
	vec3 normal,
	vec3 geo_normal,
	uint cluster_idx,
	uint material_id,
	int shadow_cull_mask,
	vec3 view_direction,
	vec3 albedo,
	vec3 base_reflectivity,
	float specular_factor,
	float roughness,
	int surface_medium,
	bool enable_caustics,
	float direct_specular_weight,
	bool enable_polygonal,
	bool is_gradient,
	int bounce,
	out vec3 diffuse,
	out vec3 specular,
	out uint out_restir_id_packed,
	out uvec2 out_restir_res_packed,
	out float dbg_w_sum,
	out float dbg_w_selected,
	out float dbg_scale,
	out int dbg_M,
	out int dbg_kind,
	out int dbg_light_id,
	out int dbg_source)
{
	diffuse = vec3(0);
	specular = vec3(0);
	out_restir_id_packed = 0u;
	out_restir_res_packed = uvec2(0);
	dbg_w_sum = 0;
	dbg_w_selected = 0;
	dbg_scale = 0;
	dbg_M = 0;
	dbg_kind = 0;
	dbg_light_id = -1;
	dbg_source = -1;

	const int RESTIR_DI_M_CLAMP = 8; // Aggressive clamp to reduce temporal bias and ghosting
	int N = int(clamp(global_ubo.pt_restir_candidates, 1.0, 32.0));

	float w_sum = 0.0;
	int M = 0;

	float selected_weight = 0.0;
	float selected_p_hat = 0.0;
	uint selected_kind = RESTIR_DI_KIND_NONE;
	uint selected_light_id = 0u;
	vec2 selected_y_pos = vec2(0);
	int selected_source = -1; // 0=current candidates, 1=temporal, 2=spatial

	// Sun is now handled separately in direct_lighting.rgen (not as a ReSTIR candidate)
	// because ReSTIR samples ONE light per pixel, but sun should contribute to ALL sunlit pixels.
	// Including sun in ReSTIR causes flat, ambient lighting when polygon lights are also enabled.
	bool enable_sun = false;  // Disabled - sun handled separately
	if(!enable_polygonal && !enable_sun)
		return;

	// TEA-based RNG for ReSTIR DI candidate generation - local to this function
	uvec2 tea_rng_state = uvec2(ipos.x + ipos.y * 32768u, global_ubo.current_frame_idx);
	#define RESTIR_DI_GET_RNG() (tea_rng_state = encrypt_tea(tea_rng_state), float(tea_rng_state.x) / float(0xffffffffu))

	// 1) Per-pixel candidate generation (unshadowed) + reservoir sampling.
	if(enable_polygonal)
	{
		for(int i = 0; i < N; i++)
		{
			float rng_light_sel = RESTIR_DI_GET_RNG();
			vec2 y_pos = vec2(RESTIR_DI_GET_RNG(), RESTIR_DI_GET_RNG());
			float rng_resample = RESTIR_DI_GET_RNG();

			uint cand_light_id;
			if(!restir_di_pick_polygonal_light_uniform(cluster_idx, rng_light_sel, cand_light_id))
				continue;

			float p_hat;
			if(!restir_di_evaluate_polygonal_unshadowed(
				cluster_idx,
				position,
				normal,
				geo_normal,
				view_direction,
				base_reflectivity,
				specular_factor,
				roughness,
				direct_specular_weight,
				cand_light_id,
				y_pos,
				p_hat))
				continue;

			float w = p_hat;
			w_sum += w;
			M += 1;

			// Weighted reservoir sampling.
			if(w > 0.0 && rng_resample < (w / max(w_sum, 1e-6)))
			{
				selected_weight = w;
				selected_p_hat = p_hat;
				selected_kind = RESTIR_DI_KIND_POLYGONAL;
				selected_light_id = cand_light_id;
				selected_y_pos = y_pos;
				selected_source = 0;
			}
		}
	}

	// Optional sun candidate: include as an additional ReSTIR DI candidate so its
	// visibility/noise can benefit from temporal/spatial reuse.
	if(enable_sun)
	{
		vec2 sun_y_pos = vec2(RESTIR_DI_GET_RNG(), RESTIR_DI_GET_RNG());
		float rng_resample = RESTIR_DI_GET_RNG();

		float p_hat;
		if(restir_di_evaluate_sun_unshadowed(
			cluster_idx,
			position,
			normal,
			geo_normal,
			view_direction,
			base_reflectivity,
			specular_factor,
			roughness,
			sun_y_pos,
			p_hat))
		{
			float w = p_hat;
			w_sum += w;
			M += 1;
			if(w > 0.0 && rng_resample < (w / max(w_sum, 1e-6)))
			{
				selected_weight = w;
				selected_p_hat = p_hat;
				selected_kind = RESTIR_DI_KIND_SUN;
				selected_light_id = 0u;
				selected_y_pos = sun_y_pos;
				selected_source = 0;
			}
		}
	}

	// Track M from current frame candidates for bias correction
	int M_current = M;

	// Bias correction tracking: store contribution data from temporal/spatial sources
	// CRITICAL: These track contributions that were CONSIDERED, not just those that WON!
	// piSum needs ALL sources for proper MIS normalization to prevent weight explosion.
	int m_temporal_considered = 0;      // M contribution from temporal source (if any)
	int m_spatial_considered = 0;       // TOTAL M contribution from ALL spatial neighbors considered
	float p_hat_at_temporal = 0.0;      // p_hat of temporal sample at current surface (for piSum)
	float p_hat_at_spatial_sum = 0.0;   // SUM of (p_hat * m) for all spatial neighbors (for piSum)

	// These track the WINNING source for the numerator (pi)
	int selected_source_m = 0;          // M of the source that produced selected sample
	float selected_source_p_hat = 0.0;  // p_hat at source surface (for RAY_TRACED bias correction)
	ivec2 selected_source_pos = ivec2(0);  // Source pixel position for RAY_TRACED visibility
	vec3 selected_source_geo_normal = vec3(0);  // Source surface geo_normal for visibility check

	bool want_temporal = (global_ubo.pt_restir_temporal > 0.5);
	bool want_spatial = (global_ubo.pt_restir_spatial > 0.5);

	// Cache current hit identity for robust reuse rejection.
	uvec2 restir_prim_curr = texelFetch(TEX_PT_VISBUF_PRIM_A, ipos, 0).xy;

	// Find out if this pixel belongs to a checkerboard-split-path surface.
	int cb_flags = checkerboard_flags;
	bool is_checkerboarded_surface = bitCount(uint(cb_flags) & uint(CHECKERBOARD_FLAG_FIELD_MASK)) > 1;
	bool primary_is_weapon = (cb_flags & CHECKERBOARD_FLAG_WEAPON) != 0;

	// Weapons don't have valid motion vectors (they move with camera), so disable reuse on them.
	if(primary_is_weapon)
	{
		want_temporal = false;
		want_spatial = false;
	}

	// For ReSTIR DI reuse, keep reprojection within the same checkerboard field (reference behavior).
	// Also keep spatial reuse within the same field to avoid mixing unrelated histories.
	bool sample_across_fields = false;

	// Field bounds in previous-frame checkerboarded coordinates.
	int field_left = (ipos.x >= global_ubo.width / 2) ? (global_ubo.prev_width / 2) : 0;
	int field_right = field_left + (global_ubo.prev_width / 2);

	vec4 motion = texelFetch(TEX_PT_MOTION, ipos, 0);
	float depth_curr = texelFetch(TEX_PT_VIEW_DEPTH_A, ipos, 0).x;
	vec2 bary_curr = texelFetch(TEX_PT_VISBUF_BARY_A, ipos, 0).rg;
	float motion_length_px = length(motion.xy * vec2(float(global_ubo.width), float(global_ubo.height)));
	ivec2 ipos_prev = ivec2(0);
	bool have_prev = false;

	// Adaptive barycentric threshold: tighter for low motion, relaxed for camera rotation.
	// Base threshold of 0.05 at zero motion, up to 0.25 at fast motion (16 pixels).
	float bary_threshold = mix(0.05, 0.25, clamp(motion_length_px / 16.0, 0.0, 1.0));

	if(want_temporal || want_spatial)
	{
		// NOTE: We no longer reject purely based on motion length. The primitive/depth/normal
		// checks are sufficient to prevent ghosting, and a hard cutoff causes the "green edges"
		// pattern during camera rotation where screen-edge motion is highest.

		// Reprojection in checkerboarded coordinates (same convention as ASVGF temporal).
		vec2 pos_prev = ((vec2(ipos) + vec2(0.5)) * vec2(global_ubo.inv_width * 2.0, global_ubo.inv_height) + motion.xy)
			* vec2(global_ubo.prev_width / 2.0, global_ubo.prev_height);
		ivec2 pos_ld = ivec2(floor(pos_prev - vec2(0.5)));

		// Choose the best candidate from a small 2x2 neighborhood (bilinear reprojection).
		// This significantly reduces ghosting/streaking compared to picking a single nearest pixel.
		const ivec2 off4[4] = ivec2[4](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));
		float best_score = -1.0;
		ivec2 best_p = ivec2(0);
		for(int i = 0; i < 4; i++)
		{
			ivec2 p = pos_ld + off4[i];
			if(p.y < 0 || p.y >= global_ubo.prev_height)
				continue;
			if(p.x < field_left || p.x >= field_right)
				continue;

			uvec2 prim_prev = texelFetch(TEX_PT_VISBUF_PRIM_B, p, 0).xy;
			if(any(notEqual(restir_prim_curr, prim_prev)))
				continue;

			// Temporal reuse expects the same shading point on the same triangle.
			// Use adaptive threshold: tighter when still, relaxed during camera motion
			// to compensate for motion vector inaccuracy.
			vec2 bary_prev = texelFetch(TEX_PT_VISBUF_BARY_B, p, 0).rg;
			if(length(bary_curr - bary_prev) > bary_threshold)
				continue;

			uint cluster_prev = texelFetch(TEX_PT_CLUSTER_B, p, 0).x;
			if(cluster_prev == 0xffffu) cluster_prev = ~0u;
			if(cluster_prev != cluster_idx)
				continue;

			float depth_prev = texelFetch(TEX_PT_VIEW_DEPTH_B, p, 0).x;
			vec3 geo_prev = decode_normal(texelFetch(TEX_PT_GEO_NORMAL_B, p, 0).x);
			float dist_depth = abs(depth_curr - depth_prev + motion.z) / max(abs(depth_curr), 1e-3);
			float dot_geo = dot(geo_normal, geo_prev);
			if(depth_curr < 0)
				dist_depth *= 0.25;

			// Stricter checks to reduce temporal bleeding artifacts
			if(dist_depth > 0.05 || dot_geo < 0.95)
				continue;

			float score = (1.0 - clamp(dist_depth / 0.1, 0.0, 1.0)) * clamp(dot_geo, 0.0, 1.0);
			if(score > best_score)
			{
				best_score = score;
				best_p = p;
			}
		}

		if(best_score >= 0.0)
		{
			have_prev = true;
			ipos_prev = best_p;
		}
	}

	// 2) Temporal reuse: merge the reprojected previous-frame reservoir.
	if(want_temporal && have_prev)
	{
		uint prev_kind;
		uint prev_light_id;
		float prev_W;
		int prev_M;
		vec2 prev_y_pos;
		if(restir_di_load_prev(ipos_prev, cluster_idx, prev_kind, prev_light_id, prev_W, prev_M, prev_y_pos))
		{
			int m_i = min(prev_M, RESTIR_DI_M_CLAMP);
			float p_hat;
			bool ok = false;
			if(prev_kind == RESTIR_DI_KIND_POLYGONAL)
			{
				ok = restir_di_evaluate_polygonal_unshadowed(
					cluster_idx,
					position,
					normal,
					geo_normal,
					view_direction,
					base_reflectivity,
					specular_factor,
					roughness,
					direct_specular_weight,
					prev_light_id,
					prev_y_pos,
					p_hat);
			}
			else if(prev_kind == RESTIR_DI_KIND_SUN)
			{
				ok = restir_di_evaluate_sun_unshadowed(
					cluster_idx,
					position,
					normal,
					geo_normal,
					view_direction,
					base_reflectivity,
					specular_factor,
					roughness,
					prev_y_pos,
					p_hat);
			}

			if(ok)
			{
				float w = p_hat * prev_W * float(m_i);
				float rng_resample = RESTIR_DI_GET_RNG();

				// CRITICAL: Track temporal contribution for piSum REGARDLESS of whether it wins!
				// This is needed for proper MIS normalization to prevent weight explosion.
				m_temporal_considered = m_i;
				p_hat_at_temporal = p_hat;

				M += m_i;  // Don't clamp M during accumulation - clamping happens at storage
				w_sum += w;
				if(w > 0.0 && rng_resample < (w / max(w_sum, 1e-6)))
				{
					selected_weight = w;
					selected_p_hat = p_hat;
					selected_kind = prev_kind;
					selected_light_id = prev_light_id;
					selected_y_pos = prev_y_pos;
					selected_source = 1;

					// Track winning source info for numerator (pi) calculation
					selected_source_m = m_i;
					selected_source_pos = ipos_prev;
					selected_source_geo_normal = decode_normal(texelFetch(TEX_PT_GEO_NORMAL_B, ipos_prev, 0).x);
					selected_source_p_hat = p_hat;  // Will be validated later for RAY_TRACED
				}
			}
		}
	}

	// 3) Spatial reuse: merge neighbor reservoirs around the previous-frame reprojected pixel.
	if(want_spatial && have_prev)
	{
		int K = int(clamp(global_ubo.pt_restir_spatial_neighbors, 0.0, 16.0));
		if(K > 0)
		{
			const ivec2 neighbor_offsets[8] = ivec2[8](
				ivec2( 1, 0), ivec2(-1, 0), ivec2( 0, 1), ivec2( 0,-1),
				ivec2( 1, 1), ivec2(-1, 1), ivec2( 1,-1), ivec2(-1,-1));

			int rot = int(floor(RESTIR_DI_GET_RNG() * 8.0)) & 7;
			for(int j = 0; j < K; j++)
			{
				ivec2 nb;
				if(sample_across_fields)
				{
					ivec2 ipos_prev_flat = restir_di_checker_to_flat(ipos_prev, global_ubo.prev_width);
					ivec2 nb_flat = ipos_prev_flat + neighbor_offsets[(j + rot) & 7];
					if(nb_flat.x < 0 || nb_flat.y < 0 || nb_flat.x >= global_ubo.prev_width || nb_flat.y >= global_ubo.prev_height)
						continue;
					nb = restir_di_flat_to_checker(nb_flat, global_ubo.prev_width);
				}
				else
				{
					nb = ipos_prev + neighbor_offsets[(j + rot) & 7];
					if(nb.y < 0 || nb.y >= global_ubo.prev_height)
						continue;
					if(nb.x < field_left || nb.x >= field_right)
						continue;
				}

				float depth_nb_prev = texelFetch(TEX_PT_VIEW_DEPTH_B, nb, 0).x;
				vec3 geo_nb_prev = decode_normal(texelFetch(TEX_PT_GEO_NORMAL_B, nb, 0).x);
				float dist_depth = abs(depth_curr - depth_nb_prev) / max(abs(depth_curr), 1e-3);
				if(depth_curr < 0)
					dist_depth *= 0.25;

				// Tighter constraints for spatial reuse to reduce color bleed:
				// - Depth: 5% relative difference (was 10%)
				// - Normal: dot > 0.95 (~18 degrees, was 0.9 ~25 degrees)
				if(dist_depth > 0.05 || dot(geo_normal, geo_nb_prev) < 0.95)
					continue;

				// NOTE: For spatial reuse, we do NOT require same primitive!
				// Spatial reuse shares light samples between NEARBY DIFFERENT surfaces.
				// Only depth/normal similarity and same cluster are required.

				uint cluster_nb = texelFetch(TEX_PT_CLUSTER_B, nb, 0).x;
				if(cluster_nb == 0xffffu) cluster_nb = ~0u;
				if(cluster_nb != cluster_idx)
					continue;

				uint nb_kind;
				uint nb_light_id;
				float nb_W;
				int nb_M;
				vec2 nb_y_pos;
				if(!restir_di_load_prev(nb, cluster_idx, nb_kind, nb_light_id, nb_W, nb_M, nb_y_pos))
					continue;

				int m_i = min(nb_M, RESTIR_DI_M_CLAMP);
				float p_hat;
				bool ok = false;
				if(nb_kind == RESTIR_DI_KIND_POLYGONAL)
				{
					ok = restir_di_evaluate_polygonal_unshadowed(
						cluster_idx,
						position,
						normal,
						geo_normal,
						view_direction,
						base_reflectivity,
						specular_factor,
						roughness,
						direct_specular_weight,
						nb_light_id,
						nb_y_pos,
						p_hat);
				}
				else if(nb_kind == RESTIR_DI_KIND_SUN)
				{
					ok = restir_di_evaluate_sun_unshadowed(
						cluster_idx,
						position,
						normal,
						geo_normal,
						view_direction,
						base_reflectivity,
						specular_factor,
						roughness,
						nb_y_pos,
						p_hat);
				}

				if(!ok)
					continue;

				// Reject spatial samples with very low p_hat - they're not relevant for
				// this surface and would cause color bleed (e.g., orange lamp light
				// bleeding onto blue-lit surfaces).
				if(p_hat < 1e-4)
					continue;

				// CRITICAL: Check visibility of spatial sample at CURRENT surface.
				// This prevents the "ray of light in cave" issue where a neighbor outside
				// shares a sun sample with a surface inside where sun is completely blocked.
				// This is expensive (shadow ray per spatial neighbor) but necessary.
				if(nb_kind == RESTIR_DI_KIND_POLYGONAL)
				{
					vec3 pos_on_light;
					vec3 contrib_unused;
					float pdfw_unused;
					bool is_sky_unused;
					if(restir_di_eval_polygonal_sample(cluster_idx, position, geo_normal,
						nb_light_id, nb_y_pos, pos_on_light, contrib_unused, pdfw_unused, is_sky_unused))
					{
						Ray vis_ray = get_shadow_ray(position - view_direction * 0.01, pos_on_light, 0);
						float vis = trace_shadow_ray(vis_ray, shadow_cull_mask);
						if(vis <= 0.0)
							continue;  // Light not visible from current surface - reject
					}
				}

				float w = p_hat * nb_W * float(m_i);
				float rng_resample = RESTIR_DI_GET_RNG();

				// CRITICAL: Track ALL spatial contributions for piSum REGARDLESS of whether they win!
				// This accumulates across ALL spatial neighbors, not just the winning one.
				m_spatial_considered += m_i;
				p_hat_at_spatial_sum += p_hat * float(m_i);

				M += m_i;  // Don't clamp M during accumulation
				w_sum += w;
				if(w > 0.0 && rng_resample < (w / max(w_sum, 1e-6)))
				{
					selected_weight = w;
					selected_p_hat = p_hat;
					selected_kind = nb_kind;
					selected_light_id = nb_light_id;
					selected_y_pos = nb_y_pos;
					selected_source = 2;

					// Track winning source info for numerator (pi) calculation
					// NOTE: Do NOT clear temporal tracking! piSum needs ALL sources.
					selected_source_m = m_i;
					selected_source_pos = nb;
					selected_source_geo_normal = geo_nb_prev;
					selected_source_p_hat = p_hat;  // Will be validated later for RAY_TRACED
				}
			}
		}
	}

	if(M <= 0 || w_sum <= 0.0 || selected_p_hat <= 0.0 || selected_kind == RESTIR_DI_KIND_NONE)
	{
		dbg_w_sum = w_sum;
		dbg_w_selected = selected_weight;
		dbg_M = M;
		dbg_source = selected_source;
		return;
	}

	// -------------------------------------------------------------------------
	// RAY_TRACED Bias Correction: Validate the winning source contribution
	// by checking if the selected sample would have been valid from source surface.
	// This prevents overbrightening from samples that are "cheating" - visible
	// from current surface but not from the source surface.
	// -------------------------------------------------------------------------

	// Get light position for visibility checks (only for polygonal lights from reused sources)
	vec3 light_pos_for_bias = vec3(0);
	bool have_light_pos = false;
	if(selected_kind == RESTIR_DI_KIND_POLYGONAL && selected_source > 0)
	{
		vec3 pos_on_light;
		vec3 contrib;
		float pdfw;
		bool is_sky_light;
		if(restir_di_eval_polygonal_sample(cluster_idx, position, geo_normal,
			selected_light_id, selected_y_pos, pos_on_light, contrib, pdfw, is_sky_light))
		{
			light_pos_for_bias = pos_on_light;
			have_light_pos = true;
		}
	}

	// Validate winning source contribution (temporal or spatial)
	// Only need to check the source that actually produced the selected sample
	if(selected_source > 0 && have_light_pos)
	{
		// Approximate source surface position using current position
		// (valid since we require same primitive with similar depth/normal)
		vec3 source_pos_approx = position;  // Same primitive, close enough

		// Check if light is above source surface's horizon
		vec3 L_source = normalize(light_pos_for_bias - source_pos_approx);
		float NdotL_source = dot(selected_source_geo_normal, L_source);

		if(NdotL_source <= 0.0)
		{
			// Light is below horizon at source surface - invalidate
			selected_source_p_hat = 0.0;
		}
		else
		{
			// RAY_TRACED: Trace visibility ray from source surface to light
			Ray source_shadow_ray = get_shadow_ray(source_pos_approx - view_direction * 0.01,
				light_pos_for_bias, 0);
			float source_vis = trace_shadow_ray(source_shadow_ray, shadow_cull_mask);

			if(source_vis <= 0.0)
			{
				// Not visible from source surface - invalidate contribution
				selected_source_p_hat = 0.0;
			}
		}
	}

	// -------------------------------------------------------------------------
	// BIAS_CORRECTION_BASIC / RAY_TRACED normalization
	// Formula: W = (w_sum * pi) / (selected_p_hat * piSum)
	// where pi = PDF at source surface, piSum = sum of (PDF at each source × M)
	//
	// For RAY_TRACED mode, we already validated visibility in the code above,
	// which zeros out selected_source_p_hat if the light isn't visible from source.
	// -------------------------------------------------------------------------
	float W;
	{
		// pi = PDF of selected sample at its source surface
		// If current frame won (selected_source == 0), use selected_p_hat
		// If temporal/spatial won, use the (potentially visibility-adjusted) source p_hat
		float pi = (selected_source == 0) ? selected_p_hat : selected_source_p_hat;

		// piSum = sum of (PDF at each source × M) for all sources that contributed
		// Start with current surface contribution
		float piSum = selected_p_hat * float(M_current);

		// Add temporal contribution
		if(m_temporal_considered > 0)
		{
			// For temporal, we use p_hat_at_temporal which is the temporal sample
			// evaluated at the current surface. For proper BASIC correction, we should
			// evaluate the SELECTED sample at the temporal surface, but that's expensive.
			// This approximation works when temporal and current have similar surfaces.
			piSum += p_hat_at_temporal * float(m_temporal_considered);
		}

		// Add spatial contributions
		if(m_spatial_considered > 0)
		{
			// Similar approximation for spatial - we use the accumulated p_hat values
			// from each neighbor's sample at current surface, not selected at each neighbor.
			piSum += p_hat_at_spatial_sum;
		}

		// Apply MIS-like normalization
		float denom = selected_p_hat * piSum;
		W = (denom > 1e-6) ? (w_sum * pi / denom) : 0.0;
	}

	if(global_ubo.pt_restir_max_w > 0.0)
		W = min(W, global_ubo.pt_restir_max_w);

	// For storage, clamp M to same value used during combination for consistency
	int M_store = min(M, RESTIR_DI_M_CLAMP);

	vec3 shaded_diffuse, shaded_specular;
	float sel_vis;
	sel_vis = 0.0;
	if(selected_kind == RESTIR_DI_KIND_POLYGONAL)
	{
		restir_di_shade_polygonal_sample(
			cluster_idx,
			position,
			normal,
			geo_normal,
			view_direction,
			shadow_cull_mask,
			base_reflectivity,
			specular_factor,
			roughness,
			direct_specular_weight,
			surface_medium,
			enable_caustics,
			selected_light_id,
			selected_y_pos,
			shaded_diffuse,
			shaded_specular,
			sel_vis);
	}
	else if(selected_kind == RESTIR_DI_KIND_SUN)
	{
		restir_di_shade_sun_sample(
			cluster_idx,
			position,
			normal,
			geo_normal,
			view_direction,
			shadow_cull_mask,
			base_reflectivity,
			specular_factor,
			roughness,
			surface_medium,
			enable_caustics,
			selected_y_pos,
			shaded_diffuse,
			shaded_specular,
			sel_vis);
	}

	// If the chosen sample is fully occluded, invalidate the reservoir so we don't
	// persist a zero-contribution sample into history (matches reference behavior).
	if(sel_vis <= 0.0)
	{
		dbg_w_sum = w_sum;
		dbg_w_selected = selected_weight;
		dbg_scale = 0.0;
		dbg_M = M;
		dbg_kind = int(selected_kind);
		dbg_light_id = int(selected_light_id);
		dbg_source = selected_source;
		return;
	}

	diffuse = shaded_diffuse * W;
	specular = shaded_specular * W;

	// Final sanity check before storing: clamp and reject NaN/Inf.
	diffuse = clamp(diffuse, vec3(0), vec3(MAX_OUTPUT_VALUE));
	specular = clamp(specular, vec3(0), vec3(MAX_OUTPUT_VALUE));
	if(any(isnan(diffuse)) || any(isinf(diffuse)))
		diffuse = vec3(0);
	if(any(isnan(specular)) || any(isinf(specular)))
		specular = vec3(0);

	// Only store valid reservoir if we got non-zero contribution.
	if(luminance(diffuse + specular) > 0.0)
	{
		out_restir_id_packed = restir_di_pack_id(selected_kind, selected_light_id);
		out_restir_res_packed = restir_di_pack_reservoir(W, M_store, selected_y_pos);
	}

	dbg_w_sum = w_sum;
	dbg_w_selected = selected_weight;
	dbg_scale = W;
	dbg_M = M;
	dbg_kind = int(selected_kind);
	dbg_light_id = int(selected_light_id);
	dbg_source = selected_source;

	#undef RESTIR_DI_GET_RNG
}

void
get_sunlight(
	uint cluster_idx, 
	uint material_id,
	vec3 position, 
	vec3 normal, 
	vec3 geo_normal, 
	vec3 view_direction, 
	vec3 base_reflectivity,
	float specular_factor,
	float roughness, 
	int surface_medium, 
	bool enable_caustics, 
	out vec3 diffuse, 
	out vec3 specular, 
	int shadow_cull_mask)
{
	diffuse = vec3(0);
	specular = vec3(0);

	if(global_ubo.sun_visible == 0)
		return;

	bool visible = (cluster_idx == ~0u) || (light_buffer.sky_visibility[cluster_idx >> 5] & (1 << (cluster_idx & 31))) != 0;

	if(!visible)
		return;

	vec2 rng3 = vec2(get_rng(RNG_SUNLIGHT_X(0)), get_rng(RNG_SUNLIGHT_Y(0)));
	vec2 disk = sample_disk(rng3);
	disk.xy *= global_ubo.sun_tan_half_angle;

	vec3 direction = normalize(global_ubo.sun_direction + global_ubo.sun_tangent * disk.x + global_ubo.sun_bitangent * disk.y);

	float NdotL = dot(direction, normal);
	float GNdotL = dot(direction, geo_normal);

	if(NdotL <= 0 || GNdotL <= 0)
		return;

	Ray shadow_ray = get_shadow_ray(position - view_direction * 0.01, position + direction * 10000, 0);
 
	float vis = trace_shadow_ray(shadow_ray, shadow_cull_mask);

	if(vis == 0)
		return;

#ifdef ENABLE_SUN_SHAPE
	// Fetch the sun color from the environment map. 
	// This allows us to get properly shaped shadows from the sun that is partially occluded
	// by clouds or landscape.

	vec3 envmap_direction = (global_ubo.environment_rotation_matrix * vec4(direction, 0)).xyz;
	
    vec3 envmap = textureLod(TEX_PHYSICAL_SKY, envmap_direction.xzy, 0).rgb;

    vec3 radiance = (global_ubo.sun_solid_angle * global_ubo.pt_env_scale) * envmap;
#else
    // Fetch the average sun color from the resolved UBO - it's faster.

    vec3 radiance = sun_color_ubo.sun_color;
#endif

#ifdef ENABLE_SHADOW_CAUSTICS
	if(enable_caustics)
	{
    	radiance *= trace_caustic_ray(shadow_ray, surface_medium);
	}
#endif

	vec3 F = vec3(0);

    if(global_ubo.pt_sun_specular > 0)
    {
		float NoH_offset = 0.5 * square(global_ubo.sun_tan_half_angle);
		vec3 specular_brdf = GGX_times_NdotL(view_direction, global_ubo.sun_direction,
			normal,roughness, base_reflectivity, NoH_offset, specular_factor, F);
    	specular = radiance * specular_brdf;
	}

	float diffuse_brdf = NdotL / M_PI;
	diffuse = radiance * diffuse_brdf * (vec3(1.0) - F);
}

vec3 clamp_output(vec3 c)
{
	if(any(isnan(c)) || any(isinf(c)))
		return vec3(0);
	else 
		return clamp(c, vec3(0), vec3(MAX_OUTPUT_VALUE));
}

vec3
sample_emissive_texture(uint material_id, MaterialInfo minfo, vec2 tex_coord, vec2 tex_coord_x, vec2 tex_coord_y, float mip_level)
{
	if (minfo.emissive_texture != 0)
    {
        vec4 image3;
	    if (mip_level >= 0)
	        image3 = global_textureLod(minfo.emissive_texture, tex_coord, mip_level);
	    else
	        image3 = global_textureGrad(minfo.emissive_texture, tex_coord, tex_coord_x, tex_coord_y);

    	vec3 corrected = correct_emissive(material_id, image3.rgb);

	    return corrected * minfo.emissive_factor;
	}

	return vec3(0);
}

vec3 get_emissive_shell(uint material_id, uint shell)
{
	vec3 c = vec3(0);

	if((shell & SHELL_MASK) != 0)
	{ 
		if ((shell & SHELL_HALF_DAM) != 0)
		{
			c.r = 0.56f;
			c.g = 0.59f;
			c.b = 0.45f;
		}
		if ((shell & SHELL_DOUBLE) != 0)
		{
			c.r = 0.9f;
			c.g = 0.7f;
		}
		if ((shell & SHELL_LITE_GREEN) != 0)
		{
			c.r = 0.7f;
			c.g = 1.0f;
			c.b = 0.7f;
		}
	    if((shell & SHELL_RED) != 0) c.r += 1;
	    if((shell & SHELL_GREEN) != 0) c.g += 1;
	    if((shell & SHELL_BLUE) != 0) c.b += 1;

	    if((material_id & MATERIAL_FLAG_WEAPON) != 0) c *= 0.2;
	}

	if(tonemap_buffer.adapted_luminance > 0)
			c.rgb *= tonemap_buffer.adapted_luminance * 100;

    return c;
}

bool get_is_gradient(ivec2 ipos)
{
	if(global_ubo.flt_enable != 0)
	{
		uint u = texelFetch(TEX_ASVGF_GRAD_SMPL_POS_A, ipos / GRAD_DWN, 0).r;

		ivec2 grad_strata_pos = ivec2(
				u >> (STRATUM_OFFSET_SHIFT * 0),
				u >> (STRATUM_OFFSET_SHIFT * 1)) & STRATUM_OFFSET_MASK;

		return (u > 0 && all(equal(grad_strata_pos, ipos % GRAD_DWN)));
	}
	
	return false;
}


void
get_material(
	Triangle triangle,
	vec3 bary,
	vec2 tex_coord,
	vec2 tex_coord_x,
	vec2 tex_coord_y,
	float mip_level,
	vec3 geo_normal,
    out vec3 base_color,
    out vec3 normal,
    out float metallic,
    out float roughness,
    out vec3 emissive,
    out float specular_factor)
{
	MaterialInfo minfo = get_material_info(triangle.material_id);

	perturb_tex_coord(triangle.material_id, global_ubo.time, tex_coord);	

    vec4 image1 = vec4(1);
    if (minfo.base_texture != 0)
    {
		if (mip_level >= 0)
		    image1 = global_textureLod(minfo.base_texture, tex_coord, mip_level);
		else
		    image1 = global_textureGrad(minfo.base_texture, tex_coord, tex_coord_x, tex_coord_y);
	}

	base_color = image1.rgb * minfo.base_factor;
	base_color = clamp(base_color, vec3(0), vec3(1));

	normal = geo_normal;
	metallic = 0;
    roughness = 1;

    if (minfo.normals_texture != 0)
    {
        vec4 image2;
	    if (mip_level >= 0)
	        image2 = global_textureLod(minfo.normals_texture, tex_coord, mip_level);
	    else
	        image2 = global_textureGrad(minfo.normals_texture, tex_coord, tex_coord_x, tex_coord_y);

		float normalMapLen;
		vec3 local_normal = rgbToNormal(image2.rgb, normalMapLen);

		if(dot(triangle.tangents[0], triangle.tangents[0]) > 0)
		{
			vec3 tangent = normalize(triangle.tangents * bary);
			vec3 bitangent = cross(geo_normal, tangent);

			if((triangle.material_id & MATERIAL_FLAG_HANDEDNESS) != 0)
        		bitangent = -bitangent;
			
			normal = tangent * local_normal.x + bitangent * local_normal.y + geo_normal * local_normal.z;
        
			float bump_scale = global_ubo.pt_bump_scale * minfo.bump_scale;
			if(is_glass(triangle.material_id))
        		bump_scale *= 0.2;

			normal = normalize(mix(geo_normal, normal, bump_scale));
		}

        metallic = clamp(image2.a * minfo.metalness_factor, 0, 1);
        
        if(minfo.roughness_override >= 0)
        	roughness = max(image1.a, minfo.roughness_override);
        else
        	roughness = image1.a;

        roughness = clamp(roughness, 0, 1);

        float effective_mip = mip_level;

    	if (effective_mip < 0)
    	{
        	ivec2 texSize = global_textureSize(minfo.normals_texture, 0);
        	vec2 tx = tex_coord_x * texSize;
        	vec2 ty = tex_coord_y * texSize;
        	float d = max(dot(tx, tx), dot(ty, ty));
        	effective_mip = 0.5 * log2(d);
        }

        bool is_mirror = (roughness < MAX_MIRROR_ROUGHNESS) && (is_chrome(triangle.material_id) || is_screen(triangle.material_id));

        if (normalMapLen > 0 && global_ubo.pt_toksvig > 0 && effective_mip > 0 && !is_mirror)
        {
            roughness = AdjustRoughnessToksvig(roughness, normalMapLen, effective_mip);
        }
    } 

    if(global_ubo.pt_roughness_override >= 0) roughness = global_ubo.pt_roughness_override;
    if(global_ubo.pt_metallic_override >= 0) metallic = global_ubo.pt_metallic_override;
    
    // The specular factor parameter should only affect dielectrics, so make it 1.0 for metals
    specular_factor = mix(minfo.specular_factor, 1.0, metallic);

	if (triangle.emissive_factor > 0)
	{
	    emissive = sample_emissive_texture(triangle.material_id, minfo, tex_coord, tex_coord_x, tex_coord_y, mip_level);
	    emissive *= triangle.emissive_factor;
	}
	else
		emissive = vec3(0);

    emissive += get_emissive_shell(triangle.material_id, triangle.shell) * base_color * (1 - metallic * 0.9);
}

bool get_camera_uv(vec2 tex_coord, out vec2 cameraUV)
{
	const vec2 minUV = vec2(11.0 / 256.0, 14.0 / 256.0);
	const vec2 maxUV = vec2(245.0 / 256.0, 148.0 / 256.0);
	
	tex_coord = fract(tex_coord);
	cameraUV = (tex_coord - minUV) / (maxUV - minUV);

	//vec2 resolution = vec2(7, 4) * 50;
	//cameraUV = (floor(cameraUV * resolution) + vec2(0.5)) / resolution;

	return all(greaterThan(cameraUV, vec2(0))) && all(lessThan(cameraUV, vec2(1)));
}

// Anisotropic texture sampling algorithm from 
// "Improved Shader and Texture Level of Detail Using Ray Cones"
// by T. Akenine-Moller et al., JCGT Vol. 10, No. 1, 2021.
// See section 5. Anisotropic Lookups.
void compute_anisotropic_texture_gradients(
	vec3 intersection,
	vec3 normal,
	vec3 ray_direction,
	float cone_radius,
	mat3 positions,
	mat3x2 tex_coords,
	vec2 tex_coords_at_intersection,
	out vec2 texGradient1,
	out vec2 texGradient2,
	out float fwidth_depth)
{
	// Compute ellipse axes.
	vec3 a1 = ray_direction - dot(normal, ray_direction) * normal;
	vec3 p1 = a1 - dot(ray_direction, a1) * ray_direction;
	a1 *= cone_radius / max(0.0001, length(p1));

	vec3 a2 = cross(normal, a1);
	vec3 p2 = a2 - dot(ray_direction, a2) * ray_direction;
	a2 *= cone_radius / max(0.0001, length(p2));

	// Compute texture coordinate gradients.
	vec3 eP, delta = intersection - positions[0];
	vec3 e1 = positions[1] - positions[0];
	vec3 e2 = positions[2] - positions[0];
	float inv_tri_area = 1.0 / dot(normal, cross(e1, e2));

	eP = delta + a1;
	float u1 = dot(normal, cross(eP, e2)) * inv_tri_area;
	float v1 = dot(normal, cross(e1, eP)) * inv_tri_area;
	texGradient1 = (1.0-u1-v1) * tex_coords[0] + u1 * tex_coords[1] +
		v1 * tex_coords[2] - tex_coords_at_intersection;

	eP = delta + a2;
	float u2 = dot(normal, cross(eP, e2)) * inv_tri_area;
	float v2 = dot(normal, cross(e1, eP)) * inv_tri_area;
	texGradient2 = (1.0-u2-v2) * tex_coords[0] + u2 * tex_coords[1] +
		v2 * tex_coords[2] - tex_coords_at_intersection;

	fwidth_depth = 1.0 / max(0.1, abs(dot(a1, ray_direction)) + abs(dot(a2, ray_direction)));
}

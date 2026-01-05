/*
Copyright (C) 2024

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
// ReSTIR GI (Global Illumination) implementation based on:
// "ReSTIR GI: Path Resampling for Real-Time Path Tracing" by Y. Ouyang et al.
//
// This file provides:
// - GI reservoir data structures
// - Packing/unpacking functions
// - Reservoir combination (weighted reservoir sampling)
// - Temporal and spatial resampling helpers
// ========================================================================== //

#ifndef RESTIR_GI_GLSL
#define RESTIR_GI_GLSL

// Maximum values for packed fields
#define RESTIR_GI_MAX_M   255
#define RESTIR_GI_MAX_AGE 255

// ========================================================================== //
// GI Reservoir structure
// ========================================================================== //

struct RestirGIReservoir
{
    vec3 position;      // Position of the secondary surface sample
    vec3 normal;        // Normal of the secondary surface
    vec3 radiance;      // Incoming radiance from the secondary surface
    float weightSum;    // Sum of weights (becomes 1/pdf after finalization)
    uint M;             // Number of candidates seen
    uint age;           // Age in frames (for temporal stability)
};

// ========================================================================== //
// Utility functions
// ========================================================================== //

// Encode normal to octahedral mapping (2x16 bit)
uint encodeNormalOct(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 oct = n.z >= 0.0 ? n.xy : ((1.0 - abs(n.yx)) * sign(n.xy));
    oct = oct * 0.5 + 0.5;
    uvec2 packed = uvec2(clamp(oct * 65535.0, 0.0, 65535.0));
    return packed.x | (packed.y << 16);
}

// Decode normal from octahedral mapping
vec3 decodeNormalOct(uint packed)
{
    vec2 oct = vec2(packed & 0xFFFF, packed >> 16) / 65535.0;
    oct = oct * 2.0 - 1.0;
    vec3 n = vec3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    }
    return normalize(n);
}

// Encode radiance to LogLuv (32 bit)
uint encodeRadianceLogLuv(vec3 rgb)
{
    // Simplified LogLuv encoding for HDR radiance
    float L = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    if (L <= 0.0) return 0;
    
    float logL = log2(L + 1.0);
    logL = clamp(logL / 16.0, 0.0, 1.0); // 16 stops of dynamic range
    
    vec2 uv = rgb.xy / (rgb.x + rgb.y + rgb.z + 1e-6);
    uv = clamp(uv, 0.0, 1.0);
    
    uint packedL = uint(logL * 1023.0);
    uint packedU = uint(uv.x * 2047.0);
    uint packedV = uint(uv.y * 2047.0);
    
    return packedL | (packedU << 10) | (packedV << 21);
}

// Decode radiance from LogLuv
vec3 decodeRadianceLogLuv(uint packed)
{
    float logL = float(packed & 0x3FF) / 1023.0 * 16.0;
    float L = exp2(logL) - 1.0;
    
    float u = float((packed >> 10) & 0x7FF) / 2047.0;
    float v = float((packed >> 21) & 0x7FF) / 2047.0;
    
    // Reconstruct from chromaticity
    float denom = max(1e-6, 1.0 - u - v);
    float Y = L;
    float X = Y * u / denom;
    float Z = Y * (1.0 - u - v) / denom;
    
    // XYZ to RGB (sRGB primaries)
    vec3 rgb;
    rgb.r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
    rgb.g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
    rgb.b = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
    
    return max(rgb, vec3(0.0));
}

// ========================================================================== //
// Reservoir I/O - Pack/Unpack
// ========================================================================== //

// Create an empty reservoir
RestirGIReservoir restirGI_emptyReservoir()
{
    RestirGIReservoir r;
    r.position = vec3(0.0);
    r.normal = vec3(0.0, 0.0, 1.0);
    r.radiance = vec3(0.0);
    r.weightSum = 0.0;
    r.M = 0;
    r.age = 0;
    return r;
}

// Create a reservoir from a raw sample
RestirGIReservoir restirGI_makeReservoir(
    vec3 samplePos,
    vec3 sampleNormal,
    vec3 sampleRadiance,
    float samplePdf)
{
    RestirGIReservoir r;
    r.position = samplePos;
    r.normal = sampleNormal;
    r.radiance = sampleRadiance;
    r.weightSum = samplePdf > 0.0 ? 1.0 / samplePdf : 0.0;
    r.M = 1;
    r.age = 0;
    return r;
}

// Check if reservoir is valid
bool restirGI_isValid(RestirGIReservoir r)
{
    return r.M > 0;
}

// Store reservoir to images (A slot)
void restirGI_storeReservoirA(ivec2 ipos, RestirGIReservoir r)
{
    // Position (xyz) + unused (w)
    imageStore(IMG_PT_RESTIR_GI_POS_A, ipos, vec4(r.position, 0.0));
    
    // Normal (packed) + Radiance (packed)
    uint packedNormal = encodeNormalOct(r.normal);
    uint packedRadiance = encodeRadianceLogLuv(r.radiance);
    // Pack M and age into remaining bits
    uint packedMA = (min(r.M, RESTIR_GI_MAX_M)) | (min(r.age, RESTIR_GI_MAX_AGE) << 8);
    imageStore(IMG_PT_RESTIR_GI_NORM_RAD_A, ipos, uvec4(packedNormal, packedRadiance, packedMA, 0));
    
    // Weight
    imageStore(IMG_PT_RESTIR_GI_WEIGHT_A, ipos, uvec4(floatBitsToUint(r.weightSum), 0, 0, 0));
}

// Load reservoir from images (A slot - current frame result)
RestirGIReservoir restirGI_loadReservoirA(ivec2 ipos)
{
    RestirGIReservoir r;
    
    vec4 posData = imageLoad(IMG_PT_RESTIR_GI_POS_A, ipos);
    r.position = posData.xyz;
    
    uvec4 normRadData = imageLoad(IMG_PT_RESTIR_GI_NORM_RAD_A, ipos);
    r.normal = decodeNormalOct(normRadData.x);
    r.radiance = decodeRadianceLogLuv(normRadData.y);
    r.M = normRadData.z & 0xFF;
    r.age = (normRadData.z >> 8) & 0xFF;
    
    uvec4 weightData = imageLoad(IMG_PT_RESTIR_GI_WEIGHT_A, ipos);
    r.weightSum = uintBitsToFloat(weightData.x);
    
    return r;
}

// Load reservoir from images (B slot - previous frame)
RestirGIReservoir restirGI_loadReservoirB(ivec2 ipos)
{
    RestirGIReservoir r;
    
    vec4 posData = texelFetch(TEX_PT_RESTIR_GI_POS_B, ipos, 0);
    r.position = posData.xyz;
    
    uvec4 normRadData = texelFetch(TEX_PT_RESTIR_GI_NORM_RAD_B, ipos, 0);
    r.normal = decodeNormalOct(normRadData.x);
    r.radiance = decodeRadianceLogLuv(normRadData.y);
    r.M = normRadData.z & 0xFF;
    r.age = (normRadData.z >> 8) & 0xFF;
    
    uvec4 weightData = texelFetch(TEX_PT_RESTIR_GI_WEIGHT_B, ipos, 0);
    r.weightSum = uintBitsToFloat(weightData.x);
    
    return r;
}

// ========================================================================== //
// Core ReSTIR functions
// ========================================================================== //

// Combine two reservoirs using weighted reservoir sampling
// Returns true if newReservoir's sample was selected
bool restirGI_combineReservoirs(
    inout RestirGIReservoir reservoir,
    RestirGIReservoir newReservoir,
    float random,
    float targetPdf)
{
    // Calculate RIS weight for the new sample
    float risWeight = targetPdf * newReservoir.weightSum * float(newReservoir.M);
    
    // Update candidate count
    reservoir.M += newReservoir.M;
    
    // Update weight sum
    reservoir.weightSum += risWeight;
    
    // Probabilistically select this sample
    bool selectSample = (random * reservoir.weightSum <= risWeight);
    
    if (selectSample)
    {
        reservoir.position = newReservoir.position;
        reservoir.normal = newReservoir.normal;
        reservoir.radiance = newReservoir.radiance;
        reservoir.age = newReservoir.age;
    }
    
    return selectSample;
}

// Finalize resampling - convert weight sum to final weight
void restirGI_finalizeResampling(
    inout RestirGIReservoir reservoir,
    float normalizationNumerator,
    float normalizationDenominator)
{
    if (normalizationDenominator > 0.0)
    {
        reservoir.weightSum = (reservoir.weightSum * normalizationNumerator) / normalizationDenominator;
    }
    else
    {
        reservoir.weightSum = 0.0;
    }
}

// ========================================================================== //
// Target PDF computation
// ========================================================================== //

// Compute target PDF for a GI sample at a given surface
// This is the integrand we want to sample proportionally to
float restirGI_getTargetPdf(
    vec3 samplePos,
    vec3 sampleRadiance,
    vec3 surfacePos,
    vec3 surfaceNormal)
{
    vec3 toSample = samplePos - surfacePos;
    float dist2 = dot(toSample, toSample);
    
    if (dist2 < 1e-6) return 0.0;
    
    vec3 dir = toSample * inversesqrt(dist2);
    float cosTheta = max(0.0, dot(surfaceNormal, dir));
    
    // Luminance-weighted target function
    float luminance = dot(sampleRadiance, vec3(0.2126, 0.7152, 0.0722));
    
    // Target = luminance * cosine term * inverse square falloff
    return luminance * cosTheta / dist2;
}

// ========================================================================== //
// Jacobian computation for reservoir reuse
// ========================================================================== //

// Calculate Jacobian determinant for transforming a GI sample between surfaces
// This accounts for the change in solid angle when viewing the same sample from different positions
float restirGI_calculateJacobian(
    vec3 currentSurfacePos,
    vec3 neighborSurfacePos,
    RestirGIReservoir reservoir)
{
    // Vector from current surface to sample
    vec3 toSampleCurrent = reservoir.position - currentSurfacePos;
    float distCurrentSq = dot(toSampleCurrent, toSampleCurrent);
    
    // Vector from neighbor surface to sample  
    vec3 toSampleNeighbor = reservoir.position - neighborSurfacePos;
    float distNeighborSq = dot(toSampleNeighbor, toSampleNeighbor);
    
    if (distCurrentSq < 1e-6 || distNeighborSq < 1e-6)
        return 0.0;
    
    // Cosine terms at the sample (light) surface
    vec3 dirCurrent = toSampleCurrent * inversesqrt(distCurrentSq);
    vec3 dirNeighbor = toSampleNeighbor * inversesqrt(distNeighborSq);
    
    float cosAtSampleCurrent = abs(dot(reservoir.normal, -dirCurrent));
    float cosAtSampleNeighbor = abs(dot(reservoir.normal, -dirNeighbor));
    
    if (cosAtSampleNeighbor < 1e-6)
        return 0.0;
    
    // Jacobian = (d_neighbor^2 / d_current^2) * (cos_current / cos_neighbor)
    float jacobian = (distNeighborSq / distCurrentSq) * (cosAtSampleCurrent / cosAtSampleNeighbor);
    
    return jacobian;
}

// Validate Jacobian to reject degenerate cases
bool restirGI_validateJacobian(float jacobian)
{
    // Reject if Jacobian is too extreme (would cause fireflies or energy loss)
    return jacobian > 0.001 && jacobian < 1000.0;
}

// ========================================================================== //
// Surface similarity tests
// ========================================================================== //

// Test if two surfaces are similar enough for reservoir reuse
bool restirGI_areSurfacesSimilar(
    vec3 normalA, vec3 normalB,
    float depthA, float depthB,
    float normalThreshold,
    float depthThreshold)
{
    // Normal similarity
    float normalDot = dot(normalA, normalB);
    if (normalDot < normalThreshold)
        return false;

    // Depth similarity (relative threshold)
    float depthDiff = abs(depthA - depthB);
    float maxDepth = max(abs(depthA), abs(depthB));
    if (depthDiff > depthThreshold * maxDepth)
        return false;

    return true;
}

// Material kind mask for similarity comparisons (upper 8 bits of material ID)
#define RESTIR_GI_MATERIAL_KIND_MASK 0xFF000000u

// Extended similarity check including material ID (following Lumen's pattern)
// Material matching is important to prevent GI bleeding between different surface types
bool restirGI_areSurfacesSimilarWithMaterial(
    vec3 normalA, vec3 normalB,
    float depthA, float depthB,
    uint materialA, uint materialB,
    float normalThreshold,
    float depthThreshold)
{
    // Material ID check - surfaces with different materials should not share samples
    // This prevents GI bleeding between e.g., metallic and diffuse surfaces
    // Compare material KIND (upper bits) to allow similar materials to share
    if ((materialA & RESTIR_GI_MATERIAL_KIND_MASK) != (materialB & RESTIR_GI_MATERIAL_KIND_MASK))
        return false;

    // Normal similarity (from Lumen: 25 degree angle threshold)
    float normalDot = dot(normalA, normalB);
    if (normalDot < normalThreshold)
        return false;

    // Depth similarity (relative threshold)
    float depthDiff = abs(depthA - depthB);
    float maxDepth = max(abs(depthA), abs(depthB));
    if (depthDiff > depthThreshold * maxDepth)
        return false;

    return true;
}

// ========================================================================== //
// Neighbor offset pattern for spatial resampling
// ========================================================================== //

// Simple pattern for spatial neighbor sampling
// Uses a mix of nearby and further samples
ivec2 restirGI_getSpatialOffset(int sampleIdx, float radius)
{
    // Golden angle based spiral pattern
    const float goldenAngle = 2.39996323;
    float angle = float(sampleIdx) * goldenAngle;
    float r = sqrt(float(sampleIdx + 1) / 16.0) * radius;
    
    return ivec2(round(cos(angle) * r), round(sin(angle) * r));
}

// ========================================================================== //
// Temporal resampling offset pattern
// ========================================================================== //

// Pattern for searching around reprojected position
ivec2 restirGI_getTemporalOffset(int sampleIdx, int radius)
{
    // 3x3 pattern around center
    const ivec2 offsets[9] = ivec2[9](
        ivec2(0, 0),
        ivec2(1, 0),
        ivec2(-1, 0),
        ivec2(0, 1),
        ivec2(0, -1),
        ivec2(1, 1),
        ivec2(-1, 1),
        ivec2(1, -1),
        ivec2(-1, -1)
    );
    
    return offsets[sampleIdx % 9] * radius;
}

#endif // RESTIR_GI_GLSL

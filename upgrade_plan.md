# Quake II RTX Fork Plan: Add ReSTIR (start with DI), then open the door to new integrators

## 0) What “done” means (clear target)
**Phase 1 (ReSTIR DI MVP):**
- Replace/augment current direct-light sampling (NEE) with **ReSTIR DI** reservoirs
- Stable, debuggable, reproducible results
- Comparable or better noise vs baseline at same spp
- No major architectural dead-ends for later BDPT / ReSTIR GI

**Phase 2 (ReSTIR DI production):**
- Temporal reuse + spatial reuse
- Light sets: point, spot, emissive triangles (as available)
- Robustness: disocclusion handling, motion, dynamic lights, camera cuts

**Phase 3+:**
- ReSTIR GI (optional)
- BDPT / other integrator swaps (long-term goal)

---

## 1) Project setup and guardrails
### 1.1 Fork + build baseline
- Fork upstream Q2RTX repo
- Make sure it builds and runs unmodified on your machine
- Add a “baseline capture” mode:
  - fixed RNG seed option (per frame/per pixel)
  - screenshot dumping for A/B comparisons

### 1.2 Branch strategy
- `main`: stays runnable
- `refactor/*`: abstraction + plumbing
- `restir-di/*`: algorithm work
- Avoid giant “everything” commits; keep changes reviewable:
  - “Add struct + wiring”
  - “Replace old NEE callsite”
  - “Add reservoir update”

### 1.3 Debug toggles you will want early
Add config toggles (cvar or defines):
- `pt_restir_enable 0/1`
- `pt_restir_temporal 0/1`
- `pt_restir_spatial 0/1`
- `pt_restir_candidates N`
- `pt_restir_debug_view 0..K` (visualize reservoirs, selected light id, weights, etc.)
- `pt_restir_rng_seed X`

---

## 2) Minimal renderer abstractions (only what ReSTIR needs)
The goal: do *not* redesign the entire engine. Add narrow “contracts” that ReSTIR/BDPT can use.

### 2.1 Light abstraction (minimum viable)
Create a `Light` interface in C style (struct + function pointers) OR a tagged union + switch:
- Types: point / spot / directional (if any) / emissive triangle (eventually)
- Required operations:
  - `LightSample sample_light(xi, shading_point)`  
    returns position, normal, emitted radiance-ish payload, and **pdf**
  - `float pdf_light(sample, shading_point)` (if needed for consistency checks)
  - `float3 eval_Le(sample, wi)` (optional for DI if you bake it into sample result)

Keep it pragmatic:
- For point lights: sampling is trivial; pdf is 1 in “discrete choice space” but you must handle mixture pdfs correctly when selecting among many lights.

### 2.2 BSDF hooks (just enough)
Make sure the shading code has explicit:
- `bsdf_sample(xi, wo -> wi, returns f, pdf)`
- `bsdf_eval(wo, wi, returns f)`
- `bsdf_pdf(wo, wi)`

Even if internally it calls the same old shading logic, force the split so future integrators can reuse it.

### 2.3 Common PDF conventions (write this down!)
Pick and document:
- Are PDFs in **solid angle measure** at the shading point? (recommended)
- When sampling an area light, implement the area->solid-angle conversion correctly:
  - `pdf_omega = pdf_area * dist^2 / abs(dot(n_light, -wi))`

This one decision determines whether everything converges or becomes “compiles but cursed”.

---

## 3) Find the incision points in Q2RTX (where to plug ReSTIR)
### 3.1 Identify direct lighting path
Locate the current logic that does:
- next-event estimation / direct light sampling
- shadow ray visibility
- add direct contribution to the path throughput/accumulator

Mark the exact function(s) and callsites.

### 3.2 Create a “DirectLightingQuery” payload
At the shading point you need:
- position `p`
- geometric normal + shading normal
- `wo` (view direction at the hit)
- material/BSDF handle
- current path throughput `beta`
- pixel id / frame id (for temporal reuse)

---

## 4) ReSTIR DI data structures
### 4.1 Reservoir definition (per pixel)
In GPU memory:
- `y` : selected candidate sample (light id + sampled point + radiance payload)
- `w_sum` : sum of weights
- `W` : final reservoir weight (or store enough to reconstruct)
- `M` : number of candidates seen
- (optional) `seed` or `rng_state`
- (optional) `age` / `validity` bits

Keep it tight; you want millions of these.

### 4.2 Candidate sample definition
For DI you typically store:
- light identifier (index)
- sampled light point (position, normal) OR enough to re-sample deterministically
- candidate contribution proxy `f_hat` (often `L_i * G * f / p`, depending on your formulation)
- pdf of generation (critical if mixing strategies)

Decide early:
- **Store full sampled position** (simpler, more memory)
- Or **store re-sample seed + light index** (harder, less memory)

Start with simplest.

---

## 5) ReSTIR DI MVP: single-frame reservoirs (no temporal/spatial yet)
### 5.1 Per-pixel candidate generation
For each shading point (primary hit first; later extend to secondary hits if desired):
- Generate `N` candidates:
  - pick a light (uniform over lights, or power-weighted)
  - sample that light to get direction and pdf
  - evaluate BSDF and geometry term
  - trace shadow ray visibility
  - compute candidate weight:
    - typical: `w_i = target(x_i) / proposal_pdf(x_i)`
- Reservoir update (standard weighted reservoir sampling):
  - increment `w_sum`
  - replace reservoir sample with probability `w_i / w_sum`
  - increment `M`

### 5.2 Shading from the reservoir
After candidates:
- Use the chosen sample `y` and compute final contribution using the reservoir’s final weight `W`
- Emit a debug view that shows:
  - selected light id (color hash)
  - w_sum
  - M
  - visibility flag

### 5.3 Validate correctness before cleverness
Validation checklist:
- If `N=1`, does it match baseline direct-light sampling statistically?
- If you disable visibility checks, do you get expected brightening?
- If you force a single light in scene, do PDFs behave?

---

## 6) Add Temporal Reuse (ReSTIR DI)
### 6.1 Motion vectors and reprojection
You need a mapping from current pixel -> previous frame pixel:
- Use existing motion vectors if present (or implement basic reprojection)
- Store previous frame reservoirs in a ping-pong buffer

### 6.2 Temporal merge
At current pixel:
- Load previous reservoir from reprojected pixel
- Validate (reject if disoccluded, normal mismatch, depth mismatch, material mismatch)
- Merge previous reservoir with current reservoir:
  - treat previous selected sample as an additional candidate set with its own effective `M` and weights
  - update reservoir accordingly

### 6.3 Temporal stability rules
- If camera cut: reset history
- Clamp history length / age
- If reproject goes out of bounds: reset

---

## 7) Add Spatial Reuse (ReSTIR DI)
### 7.1 Neighborhood sampling
For each pixel:
- pick K neighbor pixels (blue noise / fixed pattern)
- fetch their reservoirs
- validate similarity (depth/normal/material)
- merge their reservoirs into current

### 7.2 Avoid obvious footguns
- Don’t reuse across edges (depth/normal thresholds)
- Limit neighbor count for performance
- Consider a screen-space radius scaled by depth

---

## 8) Multiple Importance Sampling details (make it explicit)
Even in DI, mistakes here cause “works but noisy” or “stable but biased”.

Write down and implement:
- How you choose lights: uniform vs power distribution
- Proposal pdf for a candidate = `p(select light) * p(sample point on light -> omega)`
- Target function you’re approximating: `L_i * f * G * V` (in omega measure)
- Final estimator weight for the reservoir sample

Add debug checks:
- sanity assert PDFs > 0
- visualize `pdf` heatmaps
- compare histogram of selected light ids

---

## 9) Performance plan
### 9.1 Hotspots to expect
- shadow rays (visibility)
- candidate BSDF eval
- reservoir memory bandwidth

### 9.2 Early perf tactics
- Start DI on **primary hit only**
- Use small N (e.g., 4–8 candidates)
- Pack reservoir data (16-byte aligned structs)
- Keep validation cheap and branch-minimal

---

## 10) Code architecture for future integrator swaps
While refactoring, create boundaries that future BDPT/integrators can plug into:

### 10.1 “Scene query” layer
- `trace_primary()`
- `trace_ray()` for bounce rays
- `trace_shadow()` for visibility

### 10.2 “Shading” layer
- Material evaluation through BSDF hooks
- Light sampling through Light abstraction

### 10.3 “Integrator” layer
Make integrator entrypoints explicit:
- `integrate_pixel(sample_context)`
- `integrate_path(hit_context)` (if you want secondary-bounce DI reuse later)

This is the scaffolding that lets you later:
- swap PT -> BDPT
- add MLT / SPPM / ReSTIR GI

---

## 11) Test scenes + A/B methodology
Pick a tiny set of maps/scenes:
- one with many small lights
- one with large emissive surfaces (if available)
- one outdoor / bright environment

For each:
- baseline PT with same spp
- ReSTIR DI (single-frame)
- ReSTIR DI + temporal
- ReSTIR DI + temporal + spatial

Save:
- fixed camera path
- fixed seed
- frame dumps for diffing

---

## 12) Roadmap after ReSTIR DI
### 12.1 Extend DI beyond primary hit (optional)
- Use ReSTIR DI at first-bounce hit points
- Watch perf; this multiplies work quickly

### 12.2 ReSTIR GI (bigger jump)
- You need a definition of “sample” that represents indirect paths
- More careful reuse validation
- Often paired with spatiotemporal resampling strategies and robust clamping

### 12.3 BDPT (integrator swap)
Because you already have:
- Light sampling abstraction
- BSDF sample/eval/pdf split
- Visibility queries
- PDF conventions documented

Next steps:
- store explicit path vertices
- generate eye and light subpaths
- connect and MIS-weight strategies

---

## 13) “Help me help you” checklist (what to paste back as you progress)
When you get stuck, paste:
- The exact file/function where direct lighting is computed
- Your Light struct + sampling code
- Your reservoir struct + update code
- Your PDF conventions note (area vs omega)
- A screenshot: baseline vs ReSTIR DI at same spp
- One debug view (selected light id or w_sum)

That’s enough to diagnose 90% of “it compiles but looks wrong”.

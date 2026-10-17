# Light transport

Start with [rendering into Film](../architecture.md#rendering-into-film) for the batch-level view.
This reference follows one camera sample, then explains the material, visibility, light-sampling
and medium mechanisms used along its path.

[Architecture](../architecture.md) · [Project README](../../README.md)

## One path inside a GPU thread

![A camera ray enters the path loop. A miss collects environment light and terminates. A hit evaluates the surface, absorption, emission, direct lighting and BSDF before terminating or continuing to the next bounce.](../diagrams/path-bounce.svg)

`tracePath()` completes the entire path inside a single compute thread. Hardware ray queries
accelerate intersection; shader code then fetches hit attributes and evaluates materials.
There are no separately dispatched ray-generation or closest-hit shader stages in this implementation.

Keep two quantities distinct along the path:

- `radiance`: light already collected and eventually returned to the pixel.
- `throughput`: the weight applied to later contributions after traveling along this path; initially 1.

Each bounce follows this order:

1. **Intersect:** `traceScene()` queries the TLAS and handles material visibility at candidate hits.
   A miss adds `throughput × environment` to radiance and returns.
2. **Resolve the surface:** use the hit instance and local triangle index to fetch geometry.
   `surfaceAt()` interpolates attributes, reads textures and evaluates normals and material.
   A ray cone estimates the texture footprint.
3. **Medium and emission:** apply Beer–Lambert absorption over the segment and set the interface IORs.
   Add weighted emission to radiance. When the previous bounce used NEE and this hit is an area
   emitter, an MIS weight balances the two estimators.
4. **Direct-light NEE:** when enabled and a valid light distribution exists, select a light using
   the CPU-built CDF. Sample a point on an area light or use a point light's position. Trace a shadow
   ray, evaluate the BSDF in that direction, and collect visible direct light.
5. **Sample a continuation:** `sampleBsdf()` selects reflection or transmission. Check its direction
   against the geometric normal and inspect its weight; invalid samples terminate.
   Volume transmission updates the medium stack; reflection stays in the current medium.
6. **Update path state:** multiply throughput by the BSDF sample weight, record the PDF, NEE state
   and position, offset the next ray origin and update the ray cone.
7. **Terminate or continue:** apply Russian roulette at zero-based `bounce >= 3`, compensating
   surviving paths by dividing by their survival probability. The maximum bounce count also
   terminates the loop.

The NEE shadow ray answers whether a direct-light contribution is visible. The BSDF sample separately
chooses the next path segment. MIS (multiple importance sampling) weights estimates from different
sampling strategies to avoid double-counting the area-light direct contribution.
For material, PDF, medium and texture details, see [shading details](#shading-details)
and [transmission](#transmission-and-volume-transport).

Code: [traceMain / tracePath](../../shaders/pathtracer.slang) /
[intersection and light queries](../../shaders/scene.slang) /
[material evaluation](../../shaders/material_eval.h.slang) /
[BSDF](../../shaders/bsdf.slang) /
[media](../../shaders/medium.slang).

## Shading details

The [path loop](#one-path-inside-a-gpu-thread) owns execution order.
This section records the material, visibility and sampling details used by that loop.

### Materials and BSDF

`materialAt` produces a `SurfaceMaterial` (base color, emission, metallic, roughness, occlusion) by
multiplying the glTF factors with the matching textures. The model is metallic-roughness:

- Dielectric diffuse, reflection and transmission lobes are selected per hit; metals retain reflection.
- Specular uses a GGX distribution with VNDF sampling and height-correlated Smith visibility, and
  Schlick Fresnel.
- Sub-milliperceptual roughness is treated as a **delta** mirror lobe. Its delta sample flag prevents
  applying continuous-density emitter-hit MIS to the next hit.
- A material without the PBR flag falls back to diffuse-only (`material.flags.y`).

**Emission and occlusion.** Emission is `emissiveFactor * emissiveTexture`, and the same expression
is what `lightEmission` returns for a sampled light point, so a directly hit emitter and a sampled
one agree for MIS. The occlusion map's red channel, lerped by `occlusionStrength`, scales only the
diffuse lobe; the specular lobe is left alone.

**Normal mapping.** A ray-traced hit has no screen-space derivatives, so the tangent frame comes from
Mikkelsen's derivative-free construction: `dP/du` and `dP/dv` are taken from the triangle's
world-space edges and its normal-map UVs, orthogonalised against the interpolated shading normal, and
the sampled tangent-space normal perturbs that normal. Degenerate UVs leave the interpolated normal
unchanged, and a perturbed normal that tilts past the surface plane is flipped back toward the
geometry.

**Texture LOD.** Ray tracing has no screen-space derivatives, so `Sample()`'s implicit LOD cannot
pick a mip. Instead the ray-cone footprint at the hit is converted to texture space with the
triangle's texel density (UV area per world area) and passed to `SampleGrad` as an isotropic
gradient. The footprint widens on grazing surfaces, which is what keeps those lookups from aliasing.

### Traversal and hit resolution

`traceScene` runs an inline RayQuery that skips procedural primitives. Candidate triangles pass
through `acceptSurfaceHit` for alpha and facing rules; volume entry and exit boundaries remain
traceable regardless of the material's double-sided flag. A committed hit carries distance,
instance ID, local triangle index and barycentrics. Mirrored-instance facing uses world-space
normals derived from transformed edges.

### Light sampling

Emissive geometry is flattened into world-space `Light` triangles during `finalize`, each carrying
its material index and its emissive UVs so a textured emitter can be read at the sampled point. Each instance owns a run in `triangleLights`,
with a light index or -1 for every local triangle. A hit resolves its light as
`triangleLights[instance.lightBase + localTriangle]`. Two things keep the list balanced:

- **Pruning.** A triangle whose emissive texture is black at its centroid is not added to the list.
  It still emits when a BSDF sample hits it (the emission term is unchanged); it is simply not a NEE
  target, so a mostly-black emissive map does not bury the real lights.
- **Power weighting.** Each light is weighted by its emitted radiance times its world-space area, and
  `lightCdf` is the prefix sum. `lightIndexFromCdf` draws a uniform value and reads the light off the
  interval it lands in, so a bright light is sampled more often than a dim one. `lightPdf` scales the
  uniform-on-the-triangle density by that selection probability, and `triangleLights` maps
  a hit back to its light so a directly hit emitter is weighted by exactly the same density.

Analytic `Scene::pointLights` are appended after the triangle prefix during `finalize()`. They have
no geometry and store position in `Light::p0.xyz`, radiant intensity in `p1.rgb`, and `lightTypePoint`
in `info.y`. Their CDF weights are proportional to `4 * luminance(intensity)` (the common pi factor
is omitted from both triangle and point power weights). NEE uses the discrete light-selection
probability, inverse-square falloff, and MIS weight 1: a BSDF sample cannot hit a zero-area point.
Point lights require NEE; disabling NEE leaves environment and emissive geometry contributions.

The studio rig creates a floor and one point light at the former area emitter's center. Its intensity
is the former radiance times quad area, `36 * radius^2`, to preserve the far-field intensity along
the old emitter's normal and keep brightness comparable across model scales. Shadows are hard.

### Sampling utilities

`hash32` builds the per-sample RNG state from pixel, sample index and seed; `randomFloat` is a
small counter-based PRNG. `offsetRay` nudges ray origins off the surface to avoid self-intersection.

## Transmission and volume transport

The importer supports `KHR_materials_transmission`, `KHR_materials_volume` and `KHR_materials_ior`.
GltfMaterial stores transmission, IOR, thickness classification, attenuation color/distance and texture
metadata in the shared layout. Transmission reads the linear red channel using its own UV stream.
The default IOR is 1.5; omitted attenuation distance means no absorption.

A nonzero thicknessFactor classifies a closed volume. Thin sheets use a mirrored-hemisphere
transmission lobe without macroscopic refraction. The rough BTDF uses GGX visible-normal sampling,
the refractive Jacobian and a radiance eta-squared factor; it supports Fresnel reflection and total
internal reflection. Transmission and diffuse share the dielectric layer beneath reflection.
Metals only reflect.

medium.slang tracks up to 16 nested homogeneous volumes. Absorption is
`exp(-sigma_a * worldSpaceDistance)`; only transmitted boundary crossings update the medium stack.
Both ray-based renderers preserve thicknessTexture metadata for inspection but ignore its baked
distance when transporting light. Volume meshes must be closed with consistent outward winding.
Entry and exit faces remain traceable even when doubleSided is false. Neither extension adds
participating-medium scattering.

Raster follows at most 12 transmission boundaries, shades reached surfaces with direct/ambient light,
uses fixed pixel-seeded roughness samples and explicit secondary-ray texture LOD. Binary shadows,
missing full reflection paths and no refractive caustic solver limit it to a preview.
For glass reference images use path tracing with sufficient samples. Regression checks and commands
are documented under [transmission and volume integration tests](../../tests/README.md#transmission-and-volume-integration-tests).

# Light transport

Both renderers share material evaluation and scene resources. Path tracing follows reflection
and transmission paths; Raster uses those resources for a direct-light preview. Start with
[rendering into Film](../architecture.md#rendering-into-film) for the surrounding execution model.

## Shared surface evaluation

Material evaluation combines glTF factors, textures and interpolated geometry into the surface
description used for shading. This common boundary keeps material meaning consistent across
the two renderers while allowing their lighting methods to differ.
`surfaceAt()` resolves the hit's geometry and normals; `materialAt()` produces the `SurfaceMaterial`
consumed by the scattering model. Both live in
[material_eval.h.slang](../../shaders/material_eval.h.slang).

Normal mapping derives its surface frame from geometry and the normal map's UV coordinates.
Texture filtering depends on the available footprint: primary raster fragments have screen-space
derivatives, while traced rays estimate their footprint with ray cones. The shared evaluator
supports both contexts.

The metallic-roughness model combines dielectric diffuse, reflection and transmission; metals
reflect without a diffuse or transmission lobe. Rough specular surfaces use a GGX microfacet
model. Smooth reflection is treated as a discrete event, which matters when combining sampling
strategies. Occlusion affects diffuse shading; emission remains consistent between directly hit
and explicitly sampled emitters.

## Path transport

![A camera sample follows surface interactions, collecting emission and visible direct light, accounting for absorption and continuing through reflection or transmission until termination.](../diagrams/path-bounce.svg)

A GPU thread completes a camera sample's path in `tracePath()`. Hardware ray queries in
`traceScene()` resolve intersections; shared surface evaluation supplies materials and normals.
The path carries two distinct values:

- `radiance` is light already collected for the sample.
- `throughput` weights contributions collected after the path's previous interactions.

Next-event estimation (NEE) samples a light explicitly and tests its visibility. `sampleBsdf()`
samples the bidirectional scattering distribution function (BSDF) to choose a reflection or
transmission direction. Multiple importance sampling (MIS) balances those strategies when both
can find an area emitter. Point lights can only be
sampled explicitly, so they contribute through NEE.

Absorption attenuates light as the path travels through a medium. Reflection retains the current
medium; transmitted boundary crossings update it. Depth limits and Russian roulette bound the
work spent following a path.

## Visibility

Path traversal handles alpha coverage and surface-facing rules when accepting hits. Closed-volume
entry and exit boundaries remain visible to transmission even when the material is single-sided.
This lets the transport model track a ray entering and leaving a medium.

Raster shadow queries use binary visibility, including for transparent materials. Its transmission
preview separately resolves material-aware boundary crossings. See
[Raster shading](rasterizer.md#fragment-shading) for the consequences of that approximation.

## Light sampling

CPU scene preparation registers world-space emissive triangles and analytic point lights, then
builds a power-weighted sampling distribution in `lightCdf`. GPU shading consumes that distribution
and the `triangleLights` mapping from scene triangles to registered lights. The mapping lets an
emitter hit and an explicit light sample use matching probabilities for MIS.

Emission is evaluated from the material in both cases. CPU registration can omit textured
triangles from explicit light sampling; those surfaces can still emit when a path hits them.

Point lights use inverse-square falloff and have no hittable surface. Disabling NEE removes their
contribution. The studio rig adds a floor and a point light with intensity scaled to the model;
the resulting shadows are hard.

## Transmission and volume transport

The importer supports `KHR_materials_transmission`, `KHR_materials_volume` and `KHR_materials_ior`.
Transmission and diffuse share the dielectric layer beneath reflection. Thin sheets transmit
without macroscopic refraction. Nonzero thickness classifies a closed volume, allowing refracted
paths, changes in the index of refraction (IOR) at interfaces and total internal reflection.

The medium model tracks nested homogeneous volumes and applies Beer–Lambert absorption over
world-space path length. Only transmitted crossings change the medium. Volume meshes need closed
boundaries with consistent outward winding. Omitted attenuation distance means no absorption.
Thickness-texture metadata is retained for inspection; transport uses traced distance. The model
does not include participating-medium scattering.

Raster uses bounded secondary transmission tracing with direct and ambient shading at reached
surfaces. Binary shadows, incomplete reflection paths and the absence of a refractive caustic
solver limit it to a preview. Use Path tracing for glass reference images.

## Code entry points

| Source | What to look for |
| --- | --- |
| [gltf_importer.cpp](../../src/scene/gltf_importer.cpp) | glTF factors, texture registration and extension import |
| [scene.cpp](../../src/scene/scene.cpp) | Light registration, pruning and sampling distribution |
| [material_eval.h.slang](../../shaders/material_eval.h.slang) | Surface evaluation, texture footprints and light helpers |
| [scene.slang](../../shaders/scene.slang) | Path traversal, visibility and hit resolution |
| [pathtracer.slang](../../shaders/pathtracer.slang) | Path state, contribution weighting and termination |
| [bsdf.slang](../../shaders/bsdf.slang), [medium.slang](../../shaders/medium.slang) | Scattering model, medium boundaries and capacity limits |
| [rasterizer.slang](../../shaders/rasterizer.slang) | Direct-light and secondary transmission preview |
| [Transmission tests](../../tests/README.md#transmission-and-volume-integration-tests) | Generated scenes that exercise material transport |

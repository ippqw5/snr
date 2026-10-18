# Scene data

`Scene` is the CPU representation shared by import, inspection and GPU upload. It separates
reusable geometry from world-space placement and prepares the data that both renderers consume.
Start with the [architecture](../architecture.md#core-objects) for its place in the rendering flow.

## Geometry and instances

| Concept | Responsibility |
| --- | --- |
| Vertex attributes and triangles | Describe a surface in object space |
| `Primitive` | Associate a triangle range and its vertex window with one material |
| `Instance` | Place a primitive in the world through a transform |

Triangle indices are local to a primitive's vertex window. Import, GPU traversal and raster
geometry lookup share this addressing convention. Compact windows let the renderer use the same
geometry for shading and [acceleration structures](gpu-resources.md#acceleration-structures).

Instancing keeps geometry independent of placement. Position and normal transforms preserve
that distinction when a renderer resolves a world-space surface.

## Materials and textures

Scene geometry stores attributes in separate arrays. Each material texture slot has its own UV
stream because slots can select different glTF coordinate sets and texture transforms.

Texture identity includes color space: an image used as both color and linear data needs separate
texture entries. Sampler identity captures filtering and wrapping. Material records refer to these
registered resources, and GPU upload preserves those associations.

## Derived scene data

Scene preparation derives bounds, world-space lights and their sampling distribution from geometry
and instances. The [light-transport design](light-transport.md#light-sampling) explains how CPU light
registration supports GPU shading. The architecture describes the
[handoff to a GPU snapshot](../architecture.md#uploading-a-gpu-snapshot).

## Code entry points

| Source | What to look for |
| --- | --- |
| [scene.h](../../src/scene/scene.h) | CPU records, attribute arrays and scene interface |
| [gltf_importer.cpp](../../src/scene/gltf_importer.cpp) | `buildPrimitive`, `importMaterials` and glTF scene derivation |
| [scene.cpp](../../src/scene/scene.cpp) | Instance transforms, light registration and bounds |
| [scene_json.cpp](../../src/scene/scene_json.cpp) | CPU inspection output |

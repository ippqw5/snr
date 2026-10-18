# Repository guidelines

## Dependencies

- Import external libraries and assets as Git submodules under `external/`.
- Configure and integrate dependencies in `cmake/Dependencies.cmake`, keeping the root
  `CMakeLists.txt` focused on project targets.

## Rendering reference

Use `../../nv_projects/vk_gltf_renderer` as a read-only reference for glTF material extensions.
Consult its path tracer and shared material evaluation for semantics, compare them with the
Khronos specifications, and adapt the design to snr's architecture. Its rasterizer is a preview.

## Documentation

- Write documentation and diagram labels in English, following the
  [Microsoft Writing Style Guide](https://learn.microsoft.com/en-us/style-guide/welcome/).
- Architecture documentation explains responsibilities, boundaries, ownership, data flow and
  design decisions. Use key function names, variables and concise code excerpts when they clarify
  the design or help readers navigate the source. Link to code for the full implementation.
  Usage and testing pages provide the commands and procedures readers need.
- Describe the current architecture by revising existing sections. Keep change history in Git
  and planned work in the roadmap.

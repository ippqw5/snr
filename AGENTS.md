# Repository Guidelines

These instructions apply to the entire repository. Follow them when modifying code or build configuration.

## External Dependencies

- Manage all third-party libraries, header-only libraries, and asset repositories as Git submodules under `external/<name>/`.
- Pin each dependency to a specific commit through the parent repository's submodule gitlink. Update that commit explicitly when upgrading a dependency.
- When adding a dependency, register both its `.gitmodules` entry and its submodule gitlink. Do not copy third-party sources into an ordinary directory.
- Do not upgrade existing dependencies as an incidental part of unrelated work.
- Do not modify third-party sources inside submodules directly. If a modification is necessary, first explain the reason and how it will be maintained.

## CMake

- Keep dependency checks, third-party targets, and integration configuration in `cmake/Dependencies.cmake`.
- Keep the root `CMakeLists.txt` focused on the project's own targets and link relationships.
- CMake configuration and builds must not download dependencies from the network.
- Do not acquire dependencies through `FetchContent`, `ExternalProject`, `file(DOWNLOAD)`, download scripts, or build-time `git clone` commands.
- Apply the same rules to transitive dependencies. Configure third-party projects to use local submodule paths and disable automatic dependency downloads.
- Check required files with `snr_require_submodule(name file)`. If files are missing, fail immediately with the submodule initialization command. Do not fall back to automatic downloads.
- Compilers, CMake, Ninja, Python, and the Vulkan SDK are preinstalled build tools and toolchains. Locate them through existing paths or `find_package`.

## Dependency Change Checks

- Ensure URLs in `.gitmodules` point to official upstream repositories, not local paths or build caches.
- Ensure the parent repository records a submodule gitlink with mode `160000` and the intended commit.
- Use `git submodule update --init --recursive` to initialize dependencies in a fresh clone.
- After changing dependencies or CMake configuration, reconfigure and build the project. Confirm that compilation uses sources under `external/`.
- Update the dependency and build instructions in the README accordingly.

## Rendering Reference

- Use `../../nv_projects/vk_gltf_renderer` as a local reference for glTF material extensions and rendering behavior.
- Consult its path tracer and shared material evaluation for material semantics; its rasterizer is a preview approximation.
- Compare against the Khronos extension specifications. Adapt the implementation to this repository's shared C++/Slang layouts and rendering architecture.
- Treat the reference repository as read-only unless changes there are explicitly requested.

## Documentation and development workflow

- Use English for documentation and diagram labels, retaining exact code identifiers.
- Each topic has one primary home: `README.md` for requirements, build and quick start;
  `docs/usage.md` for launch modes, interactive controls, CLI options and example image reproduction;
  `docs/architecture.md` for the system overview, core objects, execution, ownership,
  output, reset rules and code map; `docs/reference/` for scene data, GPU layouts/resources,
  light transport, rasterizer and presentation implementation details; `tests/README.md`
  for renderer smoke tests, integration tests and manual GUI regression checks.
  Introduce project concepts in the architecture before linking to detailed references.
  Link to the owning section instead of repeating its explanation.
- Before editing, read the relevant guide section and implementation, including callers.
  Identify affected data owners, CPU/GPU boundaries and invalidated results. Verify contracts
  against code; do not treat an architecture description as proof of current behavior.
- When documented behavior or design reasons change, update their owning section and affected
  diagrams in the same change. Remove or merge superseded explanations. Local refactors do not
  need a documentation log. Keep implemented behavior separate from the README roadmap.
- Follow the build instructions in `README.md` and the renderer tests in `tests/README.md`.
  For UI or accumulation changes, exercise the affected transitions from the architecture's
  reset table. Match checks to the change; documentation-only work needs link and diagram checks,
  not a renderer build.
- Diagram source and regeneration instructions live in `docs/diagrams/README.md`.
  Keep generated SVGs with their sources. This optional local tool is outside CMake and must
  not download dependencies.
- Report the behavior change, reason, effect on data flow or lifetime, and checks actually run.
  Identify remaining limitations and checks that were not run.

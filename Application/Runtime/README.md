# Runtime package boot

`aeg_application_runtime` is the package-only startup boundary used by the
generated static game executable. It reads the binary/text game manifest beside
the executable, verifies the build identity, module contract and package
SHA-256, then mounts the cooked package.

For every declared startup Pipeline asset the bootstrap decodes the binary
pipeline payload and calls the same `prepare_pipeline_document()` path used by
the Editor and tooling. The preparation uses the static module's `TypeCatalog`
and registries, so an asset that references an unknown Pipeline, Feature or
Pass is rejected before the first frame. Runtime code does not read TOML source
documents, source metadata, or Editor/Tooling directories.

Preparation is followed by runtime activation: each authored
`PipelineInstanceConfiguration` preserves its document `PipelineScope`, is
bound to a `PipelineScopeHost`, compiled through the registered
`PipelineRegistry`, and activated before `initialize()` returns. Every frame
executes those hosts through the shared `PipelineScheduler`; shutdown
deactivates them before unloading the static module. This keeps package-only
launch on the same host/compiler/scheduler path used by Editor preview.

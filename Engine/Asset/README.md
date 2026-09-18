# Engine Asset Runtime Boundary

`aeg_asset` is the runtime side of cooked asset identity. It owns UUID parsing,
the stable `AssetKind` values, the cooked-envelope schema and codec, package
index validation, session-local UUID handles, and neutral payload conversion.

This target has no dependency on Tooling, TOML, Editor, cgltf or stb. Tooling
writers call the Engine-owned `encode_cooked_asset()` through a small facade;
the runtime consumes the same verified bytes from a package. Package index
identity, envelope identity, kind and schema version must agree before
`read_cooked()` returns an asset.

The boundary is intentional: source metadata, importers, cooker caches and
package writing belong to `Application/Tooling`; runtime identity, serialized
asset ABI, package reading and payload decoding belong here. An asset being
produced by an Application tool does not make its runtime contract an
Application dependency.

`CompositeFeature` remains an architecture and review convention. The runtime
asset layer does not impose an additional code-level restriction for it.

# Runtime package boot

`aeg_application_runtime` is the package-only startup boundary used by the
generated game executable. It resolves the selected binary manifest from the
output `current.toml` selector, verifies the build identity, module contract and
package SHA-256, loads the Cooked Feature module bundle from `module/`, then
mounts the cooked package. The executable does not compile or embed the game
module sources.

Game sources consume `public/game/game_runtime_bootstrap.hpp` through
`AEG::GameRuntime`. The engine host header `runtime_bootstrap.hpp` adds the
renderer/upload resource adapter for internal hosts only. Both use the same
bootstrap implementation; the game target receives neither Pipeline nor
Concurrency headers or compile requirements. Concurrency is an engine-internal
implementation policy and does not constrain game-authored threading or coroutines.

`runtime_asset_resolver.hpp` and `runtime_asset_upload_host.hpp` are Runtime-private
implementation headers. The public `runtime_bootstrap.hpp` surface carries only a
forward-declared `RuntimeAssetUploadHost*`; ShaderProgram, RHI handle, RenderGraph,
and uploader dependencies stay behind the Runtime private include boundary and are
not part of the `AEG::GameRuntime` authoring surface.

The private `RuntimeAssetResolver` follows the same boundary: it borrows an opaque
`RuntimeAssetUploadHost*` and includes the complete ShaderProgram/RHI upload host
only in its implementation and explicit Runtime host owners. The resolver returns
an invalid-state result when a caller tries to replace or clear the host while any
GPU resource cache is live; callers must first invoke
`retire_gpu_resources()` after the current frame has ended. Rebinding is accepted
after retirement, and the resolver never copies or owns that host contract.

The `aeg_application_runtime` target publishes only the Runtime, Foundation, Asset,
Module, Input, Platform, and Feature include requirements. Renderer, Resource,
RenderGraph, Pipeline, Engine Runtime, Application Platform, RHI, Shader, and
uploader paths are private implementation usage requirements; native tests and
engine hosts that include those contracts link their owning targets explicitly.

The private upload host publishes the fixed material shader contract as a
version and the hash read through Shader's private `ShaderPipelineLayoutAccess`
bridge. Runtime material resolution validates the host UUID, contract token, and
borrowed private layout before entering the GPU path; a missing token is not-ready
and a hash mismatch is an invalid host contract. `ShaderProgram` and layout
ownership remain private.
The host also publishes a non-zero `material_contract_epoch`. Runtime material
binding and parameter caches include that epoch, so a descriptor-compatible
Shader/layout/binding contract change cannot reuse an older GPU material; PSO
metadata replacements within the same epoch may continue to reuse descriptors.

The borrowed `ShaderPipelineLayout` also publishes its ShaderManager cache epoch.
Runtime rejects a layout contract whose epoch is retired, so material descriptor and
parameter caches fail closed across Shader/PSO cache rebuilds rather than reusing stale
pipeline-layout state.

Deferred-lighting, light-culling and post-process callbacks apply the same host
`variant_cache_epoch` check to their published Shader/PSO profile contracts. A callback result
from an older profile cache epoch is returned as `NotReady` before graph resources or descriptors
are consumed.

Non-material mesh, texture, object and lighting caches use the host's monotonic
`resource_cache_epoch` together with device/frame/graph identities. Mutating a borrowed host in
place while these GPU resources are live is rejected; retirement remains the required boundary
before accepting a new resource generation.

Neutral mesh payload decoding remains in `Engine/Asset`; conversion to the
renderer vertex ABI is owned by `runtime_mesh_vertex.*` in this target.

For every declared startup FeatureGraph asset the bootstrap stages the AEGF
payload through `aeg::application::runtime::FeatureGraphRuntimeHost`, validates the registry contract
digest, and publishes a last-good FeatureRuntime generation before the first
frame. Runtime code does not read TOML source documents, source metadata, or
Editor/Tooling directories. This remains the project's compile and publication
path; the frame/render bootstrap owns the technical frame infrastructure.

FeatureGraph publication is followed by clock-driven FeatureRuntime execution.
RuntimeBootstrap enables five independently owned domain workers through the
private Concurrency adapter. Clocks submit owning callback snapshots; the
Runtime owner pumps completions and commits state and output together. Frame
ticks do not wait for ordinary Feature callbacks to return.
The frame/render profile retains its internal technical host, scheduler and
RenderGraph projection. Its Frame Stages, Artifact bridges, pinned plans and
execution leases govern frame execution and retirement; business invocation
identity comes from committed FeatureRuntime output. Shutdown cancels
queued Feature work and joins running callbacks before invoking the game stop
callback or releasing module/context/frame infrastructure. Running callbacks
must return to allow shutdown to finish.

After pumping accepted Render Feature outputs, bootstrap captures an owning
snapshot and builds its real RenderGraph steps at the existing post-build
boundary, after frame preparation and before graph execution. The hook receives
the textures resolved by frame preparation and runs before the previous readback
hook. The owned host's shared color binding
is `Renderer.SceneColor`; Feature callbacks reference its stable ID and version.
They do not access RHI or construct the graph on their worker. The graph retains
the accepted snapshot until frame retirement. V1 covers color clear and texture
copy and instance-owned GPU history. Bootstrap reconciles live resource owners
after graph reset even when no new output exists. History publishes only after
successful graph execution; shutdown drains host frame leases before destroying
the store or device, including borrowed history-only hosts with no resolver GPU
assets. RenderDrawScene now consumes the engine-bound Runtime.Scene slot from
an independently registered World view. Exactly one extracted scene provides
the default slot; frames without Draw do not require scene extraction. The
lowerer reuses Opaque mesh/material/object/lighting recording with explicit
color/depth Load/Clear and a validated fixed material attachment ABI. Optional
external D32Float depth uses the existing Renderer.SceneDepth binding and a
host-owned color+depth PSO beside the primary color-only PSO. Both share the
same shader/layout and cached material descriptors; warm resolve uses the
current UploadContext variant without allocating duplicate material resources.
Standalone RenderDrawDepthScene also requests the existing scene extraction
and SceneDepth binding, reusing the depth-only shader/PSO already owned by the
host. depth_pipeline_contract is refreshed with the borrowed native handle on
every material resolution. depth_output explicitly anchors the external D32
producer. Depth recording uses only mesh/object bindings, while Runtime still
resolves the existing fixed material assets and cache.

Directional-shadow projection reuses the same scene extraction, depth shader
and PSO. When accepted intents reference Renderer.ShadowDepth, the owned host
binds that separate external target through the existing parameterized depth
binder. SceneDepth remains the camera depth slot; `RenderView.shadow_extent`
selects an independent shadow size and zero values fall back to the scene View.
Object resolution receives the derived light-space View and
uses its existing per-View/per-object cache. RenderDrawScene.directional_shadow
also requests ShadowDepth even without a depth producer in that intent. The
existing ShadowResources service supplies frame constants and group 3 bindings;
the owned shadow PSO keeps its color/X1/D32 contract. Warm material resolution
refreshes shadow_pipeline_contract from the live UploadContext while retaining
the existing descriptors and material parameter buffer. Renderer-internal
Forward/Deferred graph and Feature paths honor the independent extent. Runtime
and Editor preparation expose the first view through the legacy
`Renderer.ShadowDepth` identity and additional views through
`renderer::make_view_scoped_render_resource_id`; each binding uses that View's
shadow extent, so external ShadowDepth resources cannot alias across views.

Runtime post-build then groups Feature output by scene View. The first View keeps canonical
SceneColor/SceneDepth and feature resource identities; later Views receive scoped output
references and feature-texture aliases, while the View id becomes the temporal history key.
This keeps authored output execution and temporal allocations isolated without exposing the
RenderGraph Pass registry through the Runtime public surface.

Lazy upload caches track recording and successful batch submission separately.
Lost pending mesh, texture or lighting uploads are recorded again using fresh
staging and the same native allocation/view, preserving cached descriptors.
Arbitrary Draw programs, temporal role extensions and the full renderer
migration remain subsequent work.

The runtime asset cache also accepts the `AEGF` v1 Font payload and retains its
validated CPU bytes for a host-side text system. GPU atlas generation and
`TextRenderer` draw submission remain separate renderer work.

## Release smoke

The external project gate configures a copied game template, cooks a package, and launches the executable with `--headless --offscreen --frames 120 --trace`. The delivery directory contains `MyGameExecutable.exe`, `current.toml`, and the selected `versions/` bundle, then relaunches without Source or Editor files. A failed second Cook must leave `current.toml` unchanged, and a negative launch with the selected `module/GameModule.dll` removed must fail before the first frame.

```powershell
ctest --test-dir build -C Debug --output-on-failure -R "^aeg_game_project_runtime$"
```


## Internal Feature inspection and Process control

Editor-owned Runtime Inspect snapshots may attach the authored `.aegfeature` source path
after the Runtime host resolves an active asset UUID. The path is optional and remains empty
for package-only Runtime hosts; Runtime execution never reads Editor source documents.

`aeg_feature_end_to_end_smoke` loads a real cooked, non-empty Editor FeatureGraph
through the dynamic module delivery. It advances two Snapshot waves with the
existing injected clock, checks routed messages and actual Render invocations,
then runs the captured values through RenderGraph after Bootstrap shutdown.
One graph root is a real Process Feature with an explicit state subgraph; the
test suspends and resumes it through the owning generation/ProcessRevision
command before consuming the next Snapshot wave.
The shared GPU fixture verifies two 4×4 images, Feature work provenance and
output retirement at frame drain.

FeatureGraphRuntimeHost resolves an authored asset/node through its published
provenance and copies the actual live owner tree, including nested Composite
owners and independently owned Process state. Snapshots own their contract,
parameters, state and optional genuine committed Render invocation/receipts.
General invocation diagnostics cover open, issued, committed, failed and retry
facts independently of Render output.

Owner-thread Process commands check generation, authored membership, exact
instance/node/digest/resource epoch and Process revision, then delegate to the
existing Runtime lifecycle. Ordinary state commits do not stale a lifecycle
command; transitions and republication do. RuntimeFeatureControl forwards to
the Bootstrap-owned Host with no state or mutable Runtime getter. It is an
engine-only header, excluded from AEG::GameRuntime; stopped hosts reject calls.

The existing cooked Owned clustered fixture also tests a real nonempty GPU
wave 2 followed by Process suspend/cancel. Empty consumer commits retain their
real preceding demand and receipts, while fresh frames have zero Feature work
and successful default readback. Resume requires a newly completed producer
wave 3 and both consumer receipts; GPU provenance is compared per owner since
consumer state versions advance independently. Main Editor Apply/Inspect and
full renderer/ABI migration remain subsequent work.

The Runtime Inspect host and process-control facade are engine-owned forwarding
services. `feature_graph_runtime_host.hpp` and `runtime_feature_control.hpp`
therefore live under `Application/Runtime/private`; the public Runtime and
GameRuntime surfaces do not propagate either header. Editor and Runtime
implementation targets that consume these services opt into the private include
directory explicitly.


Owned clustered publication and Process modes additionally cover effective
DirectionalLight shadow through the real cooked scene/package. Receiver and
caster share the same quad/material/texture assets with independent scene UUID
relocations. Neutral camera/directional/culling intents and two typed consumers
use the existing target binder, shadow/clustered PSOs and post-processing.
A dark receiver center and lit sample are checked with the existing PointLight
oracle plus directional Lambert visibility at both exposures, alongside the
current per-owner receipts/tokens and successful empty waiting frames. All three
shadow modes use the same internal Host control service. Readback already drains
the frame inside tick, so precise pre/post-drain native leases are verified by
the native fixture rather than by a post-tick Host snapshot. Independent shadow
resolution and the broader Main/renderer/ABI migration were subsequent work at that slice;
the current implementation status is recorded in handoff.md.

The owned depth-only Shadow/Z-prepass PSO now publishes its actual reflected layout hash,
Shader/cache epoch and attachment shape as a `RendererProfile::kShadow` contract. Its
layout stays owned alongside the PSO. The private upload host forwards that contract and
borrowed layout to RuntimeAssetResolver, which checks current profile/attachment/epoch and
the actual layout before GPU upload or warm material reuse. Removing the depth PSO clears
both returned depth contracts; a new cache generation requires a real new layout/PSO.
These values remain Renderer metadata, and the Shader/RHI upload host stays Runtime-private.

The owned GBuffer PSO likewise publishes its actual reflected layout and cache epoch with
all three ordered color formats, depth and sample count as a `kGBuffer` contract. Its
layout remains owned until PSO retirement. RuntimeAssetResolver validates the current
profile, ordered attachment and live layout before uploads or warm reuse, clears both
contracts when the handle is removed, and accepts a new generation only with a real new
layout/PSO. Descriptor-compatible same-epoch PSO replacement retains material resources.

All View-scoped Scene Feature outputs receive the Runtime's actual private native pass
registry and current epoch. Fixed native dispatch preserves Feature provenance and prepared
commands through frame completion; expired registry hosts fail before execution.

The orchestrator no longer builds old instance RenderFeatures from pinned Pipeline plans,
forwards an instance RenderFeatureContext, or expands authored pass sequences. Its public
EngineFrameDesc no longer carries render_feature_buffers or render_feature_state. The
existing FeatureGraph compile/publish, owner commit and owning output/resource-owner capture
path still feeds the post-build projection above. That projection receives current owning
SceneSnapshots and the real resource resolver; frame user_data continues to reach Renderer
extraction and Stage inspection. The application supplies the native registry and epoch to
the Renderer lowerer directly, preserving actual per-pass Feature provenance and GPU leases.

Builtin native registration no longer installs the old executable Opaque, Forward or Deferred
contributions. Fixed technical frame stage declarations and native PassRegistry entries remain.
Scene rendering continues through the actual Feature captures and fixed native Registry.

RuntimeBootRequest.world_command_bindings explicitly maps a graph asset UUID, authored node
and exact authored instance path to a packaged Scene and stable entity UUID, plus effect/version.
Root paths are the authored parent path; descendants append the authored root node and actual
definition suffix. Bootstrap resolves the existing published host attribution and SceneInstance
entity mapping after both inputs are ready. Invalid, missing, ambiguous or duplicate routes fail
initialization before game callbacks. An empty list installs no consumer and leaves commands pending.

The fixed World.Transform v1 consumer belongs to Engine Runtime. Bootstrap attaches it to
the orchestrator before initialization; renderer-free ticks call the same consumer after external
commands and before World commit. Source state commit, per-entry Applied receipt and subsequent
snapshot publication are separate boundaries. Shutdown retires the borrowed orchestrator before
the consumer and Scene generation, preserving active-frame/drain failures for an owner retry.

Borrowed RuntimeFrameHostResources may supply the existing engine preparation callback to bind
external Scene attachments. apply_frame_desc borrows it with std::cref for the tick and retains its
noexcept contract; the host/callable must outlive that frame. The game facade only forward-declares
this engine resource bundle and exposes no RHI, Pipeline, World execution or Concurrency API.

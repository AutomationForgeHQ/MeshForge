# MeshForge

Every released version of MeshForge, newest first. A release publishes **one** section of this
file — the one whose heading matches its tag — as its release notes; for an `open` plugin those
notes are posted to Discord `#releases` automatically. Write for someone who installs the plugin,
not for the commit log.

Headings are `## <x.y.z> — <date>`. Use `Added` / `Changed` / `Fixed` / `Compatibility` /
`Known issues`, only the ones that apply.

## 0.3.0 — 2026-09-21

### Added
- **Skinned takes can be looked at in the Mesh tab.** The take list now holds skeletal meshes as well
  as static ones, so a garment that has been through a skinning step is no longer the one take of
  itself you could not see. The strip beneath says how many bones it has, and counts its triangles
  and vertices from the authored data rather than the render data, as it already did for a static
  mesh. Where a definition has made both, the skinned result is what opens — the same order the
  Content Browser tile uses.
- **An installed add-on can put its own control in the Mesh tab's bar**, and its own meshes beside
  the one on screen. MeshForge never reads what an add-on builds. The first is MeshForgeGarment's
  *Character*, which stands a body inside a garment.
- **Anything drawn beside the mesh puts the viewer into true space.** Left alone the viewer
  normalises — it centres each take on the origin and, comparing, turns and scales B to match A —
  which is right when the question is *which of these takes is better*. The moment something else is
  in the picture the question is *does this fit*, which is entirely about size and position, and
  every one of those adjustments becomes a lie. So then nothing is adjusted at all: every mesh is
  drawn at its own origin, at its own size, with its own pivot, exactly as a level would show them
  all dropped at 0, 0, 0. *Align* greys out with that as its reason, and the strip beneath reports
  each mesh's bounds centre and how much bigger B is than A — said, rather than fixed.
- **Post steps can be reordered.** Up and down arrows on each step move it with its settings and runs.
  The Post stage goes stale, since order changes what the chain makes; every output is kept. A move the
  chain could not run is not offered, and the greyed arrow says why.
- **Mesh Workflows**: a data asset that sets a definition up in one choice — which stages it uses,
  the pipeline on each and the post-processing chain, with their settings, and optionally its import
  settings. A slot holds its own copy of a pipeline or a soft pointer to a preset in another plugin's
  content; a preset from a plugin that is not installed is named, not an error. Choosing a workflow
  copies it into the definition, so editing one afterwards changes neither. The Stages tab picks,
  reapplies and saves them, and says what has changed since the workflow was applied. A new
  definition asks which workflow to start from, the project default first. Three ship with the
  plugin: *Generate from a prompt*, *Generate from your own picture* and *Edit a mesh in Blender*.
- **Stages can be switched off** per definition. A definition that starts from your own picture does
  not need Concept, one that starts from a mesh you already have does not need Concept, References
  or Mesh, and one whose takes you want to review first does not need Import. A switched-off stage is
  hidden from the Stages tab and the Settings tab, refused by every tool and left out of staleness.
  Every definition made before this keeps all five on and is not marked stale.
- A Mesh Definition's Content Browser tile shows what it made — its newest mesh, skinned or static —
  or, before anything is made, its main image, instead of the same icon on every definition. The
  thumbnail is stored in the asset and survives a restart: the mesh it shows is finished building before
  it is drawn, so it is never saved empty.
- `MeshForge.RebuildThumbnails [folder]` (and the Rebuild Definition Thumbnails agent tool) regenerates
  and saves every definition's thumbnail, and says which have nothing to show.
- A post step can start from **a step's output**: choose the step, then its latest run or any
  particular earlier run. Try a step with different settings, then carry on from the run you liked
  rather than the last one. Latest run inside a chain uses that chain's result. The default is
  unchanged and now reads "Step above, latest run".
- A **Floor** checkbox in the Mesh tab, off by default: most generated meshes keep their pivot at
  their centre, and the floor cut through the middle of them. The choice is remembered per user.
- **Edit in Blender - round trip**, a post step you finish by hand. It opens the mesh in Blender
  with any reference meshes locked beside it and a MeshForge tab in the sidebar. The chain waits
  until you press *Send back to Unreal*, then imports your edit as the step's output, restores the
  input's materials and carries on. A result sent after you stopped waiting, or while the editor was
  closed, can be picked up from the step. Blender's location is a per-user setting.
- Other plugins can add post steps without depending on MeshForge. A plugin registers a step — a
  settings class and a function — through `MeshForgeExtensions.h`, a header-only interface, and it
  appears under "From other plugins" in the Post stage's step picker. A definition opened without
  that plugin keeps the step and says which plugin it needs. A setting the plugin adds later and marks
  `SignatureOmitsDefault` does not mark existing steps stale.
- A step a person finishes can do its work inside the editor and hand back the edited asset itself.
  The chain records that asset as the step's output with nothing exported or imported again, so its
  vertex order, materials and build settings are kept. MeshForge Garment's in-editor garment sculpt
  is the first such step.
- **Edit in Blender works on a skinned mesh** as well as a static one. *Edit skinned mesh* on the step
  sends the skeletal mesh out with its skeleton, weights and morph targets, and brings back a copy of
  that same asset with its vertices moved to where Blender left them - nothing is imported, so the
  skeleton it is bound to, its weights, its morph targets and its materials are untouched. The topology
  has to come back unchanged, and the step says so, with both counts, when it does not. With the setting
  on the step works on assets, so it may follow a skinning step.
- An agent can work a step a person finishes one command at a time, through the step's
  `HandleInteractiveCommand` and MeshForge Toolset's Send Interactive Step Command. A step that runs to
  completion but has a window of its own takes commands too (`TakesCommands`), with or without a session
  (`CommandsNeedSession`).
- A post step can run on its own edit of its input instead of the input itself (`GetEditedInput`, asked
  on the game thread before the export). MeshForge Garment's Garment Studio keeps its sculpt this way.
- What belongs to one definition rather than to how things are made, such as a step's last session or an
  edit of its input, is left behind when a step is saved into a workflow and kept when a workflow is
  reapplied to the same definition (`ForgetDefinitionState`, `KeepDefinitionState`).

### Changed
- **A generator's pictures sit at the top of its settings**, straight under the prompt, instead of
  below every texture and shape setting. They are the first thing anybody checks. A post step keeps
  its own Input where it was.
- **A generator can say whether it builds from words or from a picture**, and the whole definition
  follows it: the settings show the prompt *or* the pictures, the Mesh stage asks for whichever is
  missing, the price quoted is the price of that request, and the take records only what was sent.
  Tripo is the first to offer the choice (`EMeshPipelineInput`, `UMeshForgePipeline::GetInputMode`).
  A generator that does not offer one behaves exactly as before.
- **A post step can start from a particular take of the Mesh stage.** *A step's output* listed only
  other post steps, so the first step - the one most likely to want a particular mesh - was told
  there were no steps to start from, even with a mesh freshly generated. The Mesh stage is now first
  in that list, and its finished takes are its runs: *Selected take* follows the Takes tab, or pick
  one and it stays that one. The take record names the take that was actually used, and List Post
  Inputs gives agents the same takes.
- A post step's **Advanced** actions now sit under the settings they work on, with Input and Pipeline
  after them, rather than above everything. The wardrobe skinning step put its *Sculpt in pose...*
  button above the Target Character it opens.
- **Each prompt belongs to the stage that reads it.** There is no longer one prompt above the stages:
  the Concept image stage's image pipeline has the words it draws from, the Mesh stage's generator the
  words it builds from where its provider reads text, and a retexture keeps its Style Prompt. A prompt
  shows only where it is read, first in that pipeline's settings; a definition that starts from a mesh
  shows none. Switching provider keeps a stage's prompt, and workflows keep the definition's prompts
  and never store their own. Existing definitions have their one prompt copied into their image and
  mesh pipelines when they load, and no stage goes stale.
- **The Stages tab reads at a glance.** Each stage is a box that opens and closes, numbered among the
  stages the definition uses (a definition that starts from a mesh no longer shows 4 and 5 after three
  stages it does not have), with a badge for where it stands and its status in the header. A stage
  that has run starts closed. Each post step is its own box inside Post-processing, with its last run in
  the header. A stage that has run and is up to date now says "done" rather than "ready".
- Post steps that make assets instead of geometry can now follow one another. The rule used to be
  "skeletal wardrobe creation must be last"; it is now "nothing that works on geometry may come after
  a step that makes an asset", so a skinned garment can be turned into an inventory item in the same
  chain.
- A post step's selected input can be a skeletal mesh, for steps that work on one.

### Fixed
- **A contradictory generator setting is now refused on the agent route too, before it spends.** The
  Stages tab has always checked a mesh pipeline before offering to run it, but the toolset came in
  further down and submitted regardless — so an agent could pay for a setting the vendor quietly
  ignores. The check now sits where both routes converge. Found by paying for it.
- **A post step no longer loses track of what made the mesh.** The chain used to overwrite the
  provenance after *every* step, so it answered "what ran last" rather than "what made this" — after
  a garment fit, a shirt genuinely generated on Tripo reported that it had not come from Tripo. The
  originating vendor task is now carried separately and only written by a step that actually returns
  one.
- **And it no longer offers a shortcut that would throw work away.** A vendor retextures the copy
  *it* holds and hands back its own geometry, which is free and correct while that copy is still our
  mesh — and silent data loss the moment it is not: point a wrapped garment at its originating task
  and back comes the *unwrapped* mesh, beautifully textured, with the wrap gone. A step that moves
  vertices now says so (`FMeshPipelineIO::bChangesGeometry`, on by default), the chain remembers it,
  and `UMeshPostPipeline::CanUseVendorTask` is the one place that decides. Getting it wrong in the
  safe direction costs an upload; getting it wrong the other way costs the work.

### Compatibility
- Update the MeshForge add-ons with it: MeshForgeCloud 0.3.0, MeshForgeTrellis 0.0.3, MeshForgeGarment
  0.2.0 and their toolsets. Their earlier builds were compiled against the previous MeshForge and are
  not compatible with this one.

## 0.2.1 — 2026-09-08
- Packaging fix: the release now carries everything the register allows. `BuildPlugin`'s filter excludes `Config/` and every `public_extra` path, so earlier zips shipped without them.

## 0.2.0 — 2026-09-07
- Documented that quads do not survive Unreal's Interchange import, and where they still exist for reference.
- Updated Tripo documentation to match the shipped v3 vendor surface.
- Repointed every plugin's Docs/Support URLs at kovati.dev.
- Added the garment pipeline: a generated shirt placed onto a MetaHuman, and the runner learning what a "body" is.
- Added a garment generated from the character's own picture (placed rather than fitted).
- Added Unreal-side inpainting as a separate skeletal post step.
- Added independent post-step execution with selectable saved inputs.
- Fixed healing of a definition that was saved while a job was in flight.
- Merged SurfaceForge (PR #4).

## 0.1.0 — 2026-09-01
- Initial release: the mesh pipeline (prompt/image → provider → glTF → static mesh), Meshy and TRELLIS.2 as providers, image generation as its own pipeline stage with a gallery, per-take provenance vault, retexture as the first post pass, and the staged Mesh Definition panel (Stages/Images/Mesh/Settings).
- Fixed convex decomposition voxelising at 100 (always returning one hull) and corrected Meshy's image pricing.
- Moved HTTP polling off the game thread so drawing/generating no longer freezes the editor.
- Declared per-provider options (`FMeshControl::Extra`) so vendor-specific settings don't collide with the shared control struct.
- Split MeshForge Cloud out to ask for its own keys; documented that a key belongs to the plugin that spends it.

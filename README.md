# MeshForge

Describe a prop or drop a reference image, generate it with an AI mesh provider, and get a
game-ready static mesh in your project — with its textures, materials, collision, lightmap UVs and
a pivot that sits on the floor.

<!-- forge:version -->**Version 0.3.0. Beta.**<!-- /forge:version -->

---

## What it is

MeshForge owns one pipeline end to end:

```
prompt / image  ->  provider  ->  glTF  ->  static mesh + materials + textures  ->  finished
```

It is **provider-agnostic**. MeshForge ships no generator of its own; every one is an add-on that
registers through the same interface. Today that is
[MeshForgeTrellis](https://kovati.dev/plugins/meshforge/) — Microsoft's TRELLIS.2, running locally in Docker, free —
and [MeshForgeCloud](https://kovati.dev/plugins/meshforge/), which wires up hosted providers (Meshy, Tripo) on your own
API key. Hosted services fit the same shape, and are already shipped, not just hypothetical.

An empty provider list is therefore a normal state, not a fault, and the editor says so.

## The flow

**The unit of work is an asset, not an action.** A *Mesh Definition* holds what you asked for —
prompt, reference image, quality, seed, polygon budget, and how the result should be finished — and
generation writes its results back into it.

That is what makes a definition **a recipe rather than a receipt**. With a seed recorded, the asset
alone reproduces the mesh and the downloaded file stops being precious. Edit the prompt, generate
again, and the same mesh updates rather than a second one appearing beside it.

1. **Author** — right-click ▸ Automation Forge ▸ MeshForge ▸ Mesh Definition. Give it a reference
   image, a prompt, or both.
2. **Estimate** — *What Would This Cost?* answers in seconds and says whether takes are billed. On a
   local runner they are free, so raise the variant count; on a hosted one every take is billed
   whether you keep it or not, which is why the default is one.
3. **Generate** — submit, poll, download, import and finish, in one press. Variants walk the seed,
   so several takes are different *and* each reproducible; only the first success becomes a mesh.
4. **Refine** — see the two loops below.

### Text, images, and which you need

**It depends on the provider, and MeshForge asks rather than assumes.** Each one publishes what it
accepts, so the editor can refuse a definition it cannot run *before* anything is spent, instead of
failing halfway through.

| | |
|---|---|
| An image | Always works. |
| A prompt only | Works on a provider that generates from text. On an image-only provider — TRELLIS.2 is one — it is refused with a message naming the ways out. |
| A prompt **and** an image | Only where the provider actually reads both. Where it does not, the prompt is dropped deliberately rather than sent to be silently ignored. |

An image-only provider may offer to *draw* its own reference image from your prompt, which makes a
prompt-only definition work as prompt → image → mesh. That is two hops, and the mesh model never
sees your words — it sees the picture. Useful, and not the same thing as text-guided generation.

### The two loops, and the cheap one comes first

**Generation** is the expensive loop: GPU time, or money. Use it when the *shape* is wrong — a
better prompt, a better reference, a different seed.

**Re-finishing is free**, and it is the one to reach for first. Collision, hull count, size in
centimetres, pivot, Nanite, lightmap resolution — all change in about a second, with no generation,
no download and no provider involved. A crate that is right but 55cm instead of 60cm never needs
regenerating.

The two are separate structs for exactly this reason: one is what the generator sees, the other is
what Unreal does afterwards.

### What lands

Everything one definition makes, in one folder named after it, under the output root set in
**Project Settings > Plugins > MeshForge**:

```
/Game/_Generated/Mesh/Definitions/MSD_AmmoCrate_Generated/
    Images/…              every picture drawn or added for it
    References/…          pictures a reference step made from other pictures
    Mesh/<take id>/…      one generated take: the static mesh (collision, lightmap UVs, Nanite
                          decided, sized, pivot on the base), its materials and its textures
    Post/<take id>/…      one post-processed take, a folder per step run
```

**Each take gets its own folder**, so a second take can never share, or silently keep, the first
one's materials. Takes accumulate on purpose — they cost money — and deleting a prop is still
deleting one folder.

## What it produces

The generator's half of this is a mesh. The other half is the part no provider can do for you,
because none of it is a property of the model — it is a property of the engine the mesh is going
into:

- **Collision** — convex decomposition by default, which is the only mode that gets a handle or a
  chair's legs right. Box, sphere and single-hull are there for the cases that do not need it.
- **Lightmap UVs** — a generated mesh arrives with exactly one UV set, the atlas its texture was
  baked into. Without a second one it lights wrongly in any project not running Lumen everywhere.
- **Nanite** — on, off, or automatic above a triangle threshold. Automatic is the default because
  the same prompt at Draft and at Ultra differs by two orders of magnitude.
- **Size and pivot** — a generated mesh is normalised into a unit box with no notion of real size,
  centred on nothing in particular. You give it a size in centimetres ("this crate is 60cm") and the
  pivot goes on its base so it sits on floors. This is the only place real-world size enters the
  pipeline, and getting it wrong is the single most common thing wrong with a generated asset.

All of that is **free to redo**. Changing a collision mode and re-finishing costs no GPU, no money
and no waiting. Reach for that before regenerating.

## A definition is a pipeline, and it opens into a panel

A Mesh Definition used to be one shot: a prompt, one image, a mesh. It is five stages now, each
independently re-runnable — draw the concept again without touching the mesh, re-import with
different collision without paying for generation twice.

```
1  Concept     an image pipeline and its prompt  → concept images
2  References  refinement pipelines (optional)   → reference images
3  Mesh        a mesh pipeline, and its prompt   → candidates
                where the generator reads text
4  Post        post pipelines, in order          → a better mesh
5  Import      collision, Nanite, LODs, textures → a UStaticMesh
```

**What runs today: Concept, Mesh and Import.** Stage 2 has two halves and only one of them is
outstanding: *choosing* which pictures the generator sees works and is where most of the value is;
*making more pictures to choose from* — background removal, extra angles — has no pipeline shipped
yet. On a provider whose image endpoint already removes backgrounds and draws multiple views, that
half is unnecessary rather than missing. Post, by contrast, is built and runs: steps execute
individually or as a chain, each with its own input source, and the shipped pipelines are Meshy and
Tripo retexture plus MeshForgeGarment's fit and skinning steps.

### Workflows: which stages a definition uses, and what they run

**Any stage can be switched off.** A definition that starts from your own picture does not need
Concept. One that starts from a mesh you already have needs none of the first three. One whose takes
you want to review before anything is imported does not need Import. The switches sit at the top of
the Stages tab. A stage that is off is hidden from both the Stages and the Settings tab, refused by
every tool, and left out of staleness, so editing its pipeline marks nothing after it stale. Every
definition made before the switches existed has all five on and hashes exactly as it did.

**A Mesh Workflow sets all of it in one choice.** It is a data asset holding the stage switches, the
pipeline on each stage and the post chain with their settings, and, where it matters, the import
settings. Pick one in the workflow bar and it is **copied** into the definition: it is a template,
so editing the definition afterwards changes neither. The bar then shows what has changed since, and
**Reapply** puts the workflow back while keeping the prompts, pictures, Source Mesh, takes and the
outputs of any post step that is still in its place. **Save as Workflow...** makes one from the
definition as it stands, which is the easy way to author one. A new definition asks which workflow to
start from; `Default Workflow` in the project settings is offered first.

A slot in a workflow holds either its own copy of a pipeline or a **soft pointer** to one in another
plugin's content. That is how a workflow shipped with one plugin uses steps another plugin provides
without depending on it: if that plugin is not installed, the slot is left empty and the workflow
names the plugin it needs.

| Workflow | Ships with | Stages |
|---|---|---|
| Generate from a prompt | MeshForge | Concept, References, Mesh, Import |
| Generate from your own picture | MeshForge | References, Mesh, Import |
| Edit a mesh in Blender | MeshForge | Post (Edit in Blender), Import |
| Garment for a character | MeshForgeGarment | References, Mesh, Post (fit, skinning), Import that keeps the fitted origin |
| Narrative clothing item | NP_Clothing | The garment workflow's steps, by soft pointer, then the clothing item step |

The shipped ones choose no mesh or image provider: that choice costs money on some and needs a GPU on
others, so it is yours. They are ordinary assets under each plugin's `Workflows` folder, and editable.

### Nothing blocks the editor

Drawing takes fifteen seconds to two minutes and generating takes minutes. Both start and return; a
**Work in progress** strip above the stages lists what is running, for which definition, with a
running clock, and keeps a failed job's reason where it can be read.

This is not a nicety. Drawing used to run on the game thread and pump HTTP inside the wait, which
stopped Unreal redrawing for the whole of it — indistinguishable from a hang, and somebody who sees
nothing happen presses the button again.

### Stale, not discarded

Every stage stores a hash of the inputs it was actually run with, and that hash includes every stage
before it. Edit the picture's prompt and the *mesh* goes stale, not just the concept image — because
the mesh was made from a picture that no longer exists.

### Each prompt belongs to what reads it

There is no prompt for the whole definition. A picture and the mesh made from it often want different
words, and a retexture wants its own again, so each pipeline that works from words keeps its own:
the **Concept image** stage's image pipeline has the words it draws from, the **Mesh** stage's
generator has the words it builds from when its provider reads text, and a retexture step has its
**Style Prompt**. A prompt appears only in a stage whose pipeline reads it, first in its settings; a
definition that starts from a mesh you already have shows none.

Switching a stage to another provider keeps its prompt, and a workflow never stores one - a prompt is
what is being made, not how - so applying or reapplying a workflow keeps the definition's prompts.
Definitions made before this had one prompt; it was copied into their image and mesh pipelines the
first time they loaded, and nothing went stale.

**The mesh is kept.** It cost minutes and money, and discarding it to protect a cheap invariant
would throw away the expensive thing. What must not happen is showing it as though it still matched,
and that is what the stage status is for.

### The panel

Five tabs. **Stages** shows, on each stage that takes one pipeline, that pipeline's own settings
inline, its prompt first where it reads one — choosing the local image pipeline puts a model, a
size and a step count on the row; choosing the Meshy one replaces them with a model, an aspect ratio
and a pose. Neither list is written in the panel. Both come from the same reflected properties an
agent reads.

Each stage is a box that opens and closes, numbered among the stages the definition uses, with a badge
that says where it stands - done, the one to do now, needs attention (stale or failed), or waiting -
and its status in the header so a closed stage still says it. A stage that has run starts closed.
Each post step is a box of its own inside Post-processing, with its last run in the header.

**Images** shows one picture large with a filmstrip beneath it, arrow keys to walk it, and the
buttons that decide what the generator is shown. A 168-pixel tile cannot answer the only question
anybody asks of a reference — is this worth reconstructing from — so it is not a grid.

**Mesh** is a viewport built on `FAdvancedPreviewScene`, the same one the Static Mesh editor uses,
so a generated prop is judged under the lighting everything else is judged under. Viewer on the
left, controls on the right.

**Takes** is every take the definition has ever produced, read from the library on disk rather than
from the definition — so a take somebody removed from the list to tidy it is still there, with its
prompt and its price, and can be imported again. The columns are the questions people ask, in the
order they ask them: when, what made it, how big, how long, what it cost.

**Settings** is the whole asset as a details panel, in pipeline order. That order is set explicitly
by a details customization, because Unreal does not sort categories by declaration order and reading
`1 Concept, Prompt, 3 Mesh, 5 Import, 2 References, 4 Post` is worse than reading nothing.

### Choosing what the generator sees

A main image and up to three extra views, chosen from every picture the definition holds — drawn,
added by hand, or produced by a refinement pipeline. Chosen by pointer rather than by position, so
drawing another concept cannot silently change what the mesh was made from.

**Extra views appear only where the chosen generator says it can use them.** TRELLIS.2 takes one
picture and drops the rest with a line in the log; Meshy's multi-image endpoint takes four. Three
pictures silently ignored looks exactly like three pictures used badly.

And be sceptical of them even where they are supported: extra views were tried three ways and each
made the reconstruction measurably *worse* than the single picture it came from. A model fuses
contradictory evidence rather than averaging it.

Its triangle count comes from the mesh description rather than LOD 0's render data. Under Nanite the
render data reports the *fallback* mesh, which is smaller by an order of magnitude and is not what
the asset contains — a number this plugin published wrongly once already.

## Pipelines

A pipeline is a **C++ class**, one per provider or per provider *flow*, exposing that vendor's real
settings as typed properties. Instances are assets: configure `Meshy — hero props, 4K, no remesh`
once and point forty definitions at it.

Typed rather than a bag of strings, and that is the point. A `TMap<FString, FString>` cannot show a
slider, enumerate a vendor's four topology modes, grey out an option that does not apply, or carry a
tooltip. Reflection does all of it the moment a setting is a real `UPROPERTY` — which is why the
details panel and the agent option list are generated from the same declaration and cannot drift.

| Kind | Ships today | Proven |
|---|---|---|
| Image | `Image - Local (this machine's GPU)` (MeshForgeTrellis) | 106s cold, 23s warm, free |
| Image | `Image - Meshy (nano-banana, GPT Image)`, `Image - Tripo API v3`, `Image - Tripo multi-view (4 angles)` (MeshForgeCloud) | nano-banana: 19s, 3 credits |
| Mesh | `Meshy - Standard (high quality)`, `Meshy - Smart lowpoly (T2)`, `Tripo - Photoreal (H3)`, `Tripo - Game ready (P2/P1)` (MeshForgeCloud) | measured on meshy-7 — 1,943,800 triangles for 30 credits; 6,275 for 15 on Smart lowpoly |
| Mesh | TRELLIS.2 (MeshForgeTrellis) — chosen as a provider, with its settings and no pipeline class | |
| Refine | — | |
| Post | `Edit in Blender - round trip` (MeshForge), `Meshy - retexture`, `Tripo - retexture` (MeshForgeCloud); MeshForgeGarment adds `Garment fit`, `Wardrobe skinning` and `Sculpt mesh - in the editor`, NP_Clothing adds a clothing item step | |

**An image pipeline does its own work, and that is a deliberate exception.** Everywhere else a
pipeline declares and a provider acts. A provider interface can only carry what every provider has
in common, and for image generation that is very nearly nothing — a local diffusion model wants
steps, a size and a seed; a hosted one wants a model name, an aspect ratio and a pose. Routing them
through one call meant those settings were drawn, saved, hashed for staleness, and then dropped on
the floor at the moment they mattered.

A mesh pipeline keeps the declare-and-act split, but it writes its settings into the request through
`Apply` — and it chooses the provider. Without that, the panel could say "Meshy, about 30 credits"
while generation ran on a free local container, or the reverse.

**Meshy ships two rather than one**, and the reason is a trap found on a live account:
`geometry_resolution`, `image_enhancement` and `auto_size` do not exist in Smart Topology, and
sending one is not refused — it silently switches mode and doubles the bill. One asset with half its
settings greyed out would leave that one careless click away.

Post-processing is an **array**, because anything that takes a mesh and returns one belongs there,
and a prop is often assembled from several vendors: the shape from one, the textures from another,
the topology from a third.

## Every generated picture becomes an asset

Images are ingested as real `UTexture2D`s in the definition's own `Images/` folder, appended
rather than replacing — two runs are two candidates, and on a provider with no seed a discarded one
cannot be drawn again.

Ingestion writes `FTextureSource`, so **the original pixels live inside the asset** beside the
compressed copy that renders. Sending a picture back to a provider for a second pass costs no
quality, which is what makes the Images tab safe to treat as the only copy.

## Provider options

A provider can carry more than the shared control struct does. `FMeshControl` holds what *every*
generator has — a seed, a quality, a triangle budget, a texture size — which necessarily leaves out
the things that make one provider worth choosing over another.

So a provider **declares** its own. Each option publishes a key, a type, its allowed values, a
default, a tooltip, and **which quality steps it applies to**; `List Mesh Providers` returns them
beside the capabilities, and values are set through `Control.Extra` under the same keys.

That last field is a safety feature rather than a nicety. On at least one hosted provider, an option
that does not belong to the current generation mode is **not refused** — it silently changes the mode
and doubles the price, and nothing in the reply says which mode ran. Knowing where an option applies
is what lets an editor avoid offering it somewhere it will do harm.

A provider ignores keys it did not declare, because a definition keeps its extras when its provider
changes, and refusing on another provider's key would break simply switching one to compare.

## What it deliberately does not do

- **No rigging.** A generated character comes back as one watertight surface with no skeleton and
  no part decomposition. Useful as a statue or a background figure; not an animated character.
  Pretending otherwise would mean a half-working skeletal path that is wrong for the props this is
  actually for. What MeshForge generates is always a static mesh. A post step from an add-on can
  skin one — MeshForgeGarment's wardrobe skinning binds a garment to a character's skeleton — and
  MeshForge then keeps, shows and can round-trip that skinned result through Blender, but it never
  makes a skeleton itself.
- **No level placement.** Dragging a finished mesh into a level is a person's job.

## Using it

1. Enable a provider plugin, and set it in **Project Settings > Plugins > MeshForge**.
2. Right-click in the Content Browser: **Automation Forge > MeshForge > Mesh Definition**.
3. Choose the pipelines, then write each one's prompt, add a reference image, or both. Check the
   provider's capabilities first — some generate from text, some only from an image.
4. Generate. The first successful take is imported and finished automatically.

Everything one definition makes lands in a single folder named after it, with a folder per take
inside — see [What lands](#what-lands). Deleting a prop is deleting that folder.

You can also finish a glTF you already have, with no provider and no GPU — useful for a mesh from a
web tool, a scan, or a colleague:

```
MeshForge.ImportFile C:/path/to/thing.glb SM_Thing
```

Console commands:

```
MeshForge.ListProviders       what is installed, and whether each can generate right now
MeshForge.TestConnection      check one provider (optional argument: its id)
MeshForge.RebuildThumbnails   regenerate and save every Mesh Definition's Content Browser thumbnail
                              (optional argument: a content folder, default /Game)
MeshForge.VerifyStageHashes   report definitions whose stages were saved ready but whose inputs now
                              hash differently (optional argument: a content folder)
```

## Settings

Split three ways, by who owns the answer:

| Where | What |
|---|---|
| **Project Settings > Plugins > MeshForge** | Output paths, the default provider, the default workflow, quality and finish defaults. Committed; the team shares them. |
| **Editor Preferences > Automation Forge > MeshForge** | Which provider *I* use, where Blender is for *Edit in Blender*, and whether the Mesh tab shows a floor. Never committed. A provider's key is entered on that provider's own preferences page or on the Automation Forge Keys page. |
| The OS credential vault | Secrets. Never in any `.ini`. |

## For agents

[MeshForgeToolset](https://github.com/AutomationForgeHQ/MeshForgeToolset) exposes the pipeline through the Unreal toolset registry:
list providers, author definitions, list and set pipelines, draw concept images, list and choose
pictures, estimate, generate, import, re-finish, list and apply workflows, switch stages, and list
what is running. It is a thin adapter —
deleting it changes nothing about MeshForge.

**Anything a person can do here, an agent can do**, and that is a rule rather than an accident: the
image stage is the largest quality lever in the pipeline, and a surface that could generate a mesh
but not decide what it was shown would be the wrong half.

`Set Pipeline` creates and wires and hands back the object's path; its settings are then written
with the generic object property tools. Mirroring every vendor's fields into typed signatures here
would rot the moment one of them adds a setting, and the reflected options travel in the result
already.

Estimates go through the same resolver as submissions, so the number an agent reads and the number
the panel shows cannot disagree — they did, by a factor of two, until they shared one.

## Individual post-processing steps

Each step has **Run this step**, up and down arrows, and an **Input Source** setting. The arrows move the step,
keeping its settings and runs; the Post stage goes stale, since order changes what the chain makes, and every
output is kept. A move the chain could not run - a step that works on geometry after one that makes an asset -
is not offered, and the greyed arrow says why. **Move Post Step** does the same for agents.

The input settings:

- **Step above, latest run** (the default) uses the latest saved result from the immediately preceding step. The first step uses the original mesh. A missing output blocks execution with an explanation; it never triggers a prerequisite run.
- **Original mesh** uses the definition's Source Mesh, or its selected generated mesh when the step supports GLB input.
- **Selected mesh / saved output** uses one fixed asset. **Choose saved output as input** lists every saved output of this definition, from any step and any run, including takes created before per-step history existed; the asset picker also accepts meshes from other definitions. It stays that exact asset whatever runs later.
- **A step's output** picks a step, then a run of it: **Latest run** follows the step, or a particular run stays that run. This is for trying a step with different settings and carrying on from the result that came out best rather than from whichever ran last. Both lists are in the step's row; the ids behind them are `InputStepId` and `InputTakeId`, which List Post Inputs reports.

**Run chain** executes enabled steps. A connection to the step above, or to the latest run of any step earlier in the same run, uses that run's fresh output; otherwise it requires a saved output. Explicit inputs remain explicit during a chain run. Every completed step appends a separate output and records its input; earlier assets are preserved. Steps that make assets rather than geometry - skinning, an inventory item - may be followed only by other such steps, and can be run individually.

New outputs carry stable producer step IDs. Historical outputs are matched automatically only when there is a single producer of that class; ambiguous old results can still be selected explicitly.

### Edit in Blender

**Edit in Blender - round trip** is a post step a person finishes. Running it writes the step's input
(with textures) and any **Reference Meshes** into a session folder under
`Intermediate/MeshForge/Interactive/<definition>/`, and opens Blender on them with a small add-on
loaded for that session only. The mesh is selected, in sculpt mode if you asked for it, with the
references locked beside it in the space they occupy in Unreal. The 3D view's sidebar gets a
**MeshForge** tab with **Send back to Unreal** and **Cancel the edit**.

The chain waits, and the job list says so. Sending writes `result.glb` and then `manifest.json`
(by rename, so a half-written result is never read). MeshForge imports the result as this step's
output, puts the input's own materials back slot for slot, and carries on with the next step. A
skinning step after it therefore skins what you sculpted.

- **Stop waiting** on the step leaves Blender open. A result sent later shows **Pick up result** on
  the step, and so does one sent while the editor was closed. Picking up imports it and does not run
  the steps after it.
- **The summary says whether topology changed.** Sculpting keeps it; remeshing or adding geometry
  does not, and then a retexture's UVs and a morph bake's vertex order no longer line up.
- **Blender's importer traps are handled for you:** seam vertices merged, Euler rotation mode,
  smooth shading on files without normals, and the bone-shape sphere a skinned reference brings
  hidden. Nothing is rescaled, because glTF is metres both ways.
- Blender is found under Program Files, newest first, or set in **Editor Preferences > Automation
  Forge > MeshForge > Blender Executable**. The add-on writes `blender-log.txt` into the session
  folder, which is where to look when Blender opens on an empty scene.

#### The skinned mesh

**Edit skinned mesh** on the step turns the same round trip on a skeletal mesh - the garment a skinning
step made - so it can be sculpted where it will actually deform. It goes out with its skeleton, its
weights and its morph targets, and its armature is in the scene, hidden and locked, driving the mesh.

**Nothing is imported back.** The result is a copy of the same skeletal asset with its own vertices
moved to where Blender left them, so the skeleton it is bound to, its weights, its morph targets and
its materials are the ones it went out with - no import onto a skeleton, and none of the silent
rebuild that comes with getting that wrong. The add-on reports where every vertex arrived and where it
left it, and MeshForge matches its own vertices to those by position, so it does not matter that glTF
splits vertices or that Blender merged them.

**The topology therefore has to come back unchanged**, and the step says so, with both counts, when it
does not. Adding or removing geometry on a skinned mesh means new vertices with no weights: do that on
the static mesh before skinning, and skin the result.

With the setting on, the step works on assets rather than geometry, so it may follow a skinning step
and nothing that works on geometry may follow it.

### Steps finished inside the editor

A step a person finishes does not have to leave the editor. MeshForge Garment's **Sculpt mesh - in
the editor** edits a working copy of the step's input in an editor window, and hands that saved asset
back as the step's output: nothing is written to a file or imported again, so vertex order, materials
and the mesh's build settings come out as they went in. A step says so with
`WantsInteractiveInputGlb` (no input file is written for it) and `GetInteractiveResultAsset`.

An agent can work such a step without a mouse or a keyboard. `HandleInteractiveCommand` on the step
takes one command at a time in the step's own words, and **Send Interactive Step Command** in
MeshForge Toolset reaches it: the session the chain is waiting on, or else the last one the step
started. Edit in Blender takes no commands. A step that runs to completion but has a window of its own
answers `TakesCommands`, and says with `CommandsNeedSession` that it answers without a session: MeshForge
Garment's Garment Studio on the fit step. That step is also interactive only once it has been finished in
the studio, so a chain hands the finished garment on, or waits for the studio, instead of wrapping again.

A step can also keep its own edit of its input and run on that instead: `GetEditedInput` is asked on
the game thread, on the definition's step, before the input is exported, and returns the mesh to run on,
nothing to run on the input, or a reason to refuse. The Garment Studio's sculpt is one. State like this
belongs to one definition, so `ForgetDefinitionState` drops it when the step is saved into a workflow
and `KeepDefinitionState` carries it when a workflow is reapplied to the same definition.

### Steps from other plugins

A plugin that does not depend on MeshForge can add a post step through `MeshForgeExtensions.h`, a
header-only interface: it registers a settings class and a game-thread function, and the step appears
under **Add a step... → From other plugins**. NP_Clothing's *Narrative clothing item* is one. Such a
step works on assets rather than geometry, so it follows the steps that make them; nothing that works
on geometry may come after it. A definition opened without the registering plugin keeps the step and
names the plugin it needs.

The editor tool `RunPostProcessing` accepts a zero-based `stepIndex` (`-1` runs the chain). `ListPostInputs` discovers saved outputs. Native output success is reported through `skeletalMesh` rather than `mesh`.

## Requires

Unreal Engine 5.8, with Interchange's glTF support (on by default). Editor-only; nothing here
ships in a packaged game.

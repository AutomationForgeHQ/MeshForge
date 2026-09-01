# MeshForge

Describe a prop or drop a reference image, generate it with an AI mesh provider, and get a
game-ready static mesh in your project — with its textures, materials, collision, lightmap UVs and
a pivot that sits on the floor.

**Version 0.0.1. Experimental, and not yet released.**

---

## What it is

MeshForge owns one pipeline end to end:

```
prompt / image  ->  provider  ->  glTF  ->  static mesh + materials + textures  ->  finished
```

It is **provider-agnostic**. MeshForge ships no generator of its own; every one is an add-on that
registers through the same interface. Today that is
[MeshForgeTrellis](../MeshForgeTrellis) — Microsoft's TRELLIS.2, running locally in Docker, free.
Hosted services fit the same shape.

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

Everything one prop needs, in one folder named after it:

```
/Game/_Generated/Mesh/Meshes/MSD_AmmoCrate/
    StaticMeshes/SM_AmmoCrate     collision, lightmap UVs, Nanite decided, sized, pivot on the base
    Materials/…                   PBR, from the glTF
    Textures/…                    base colour, roughness, metallic
```

Deleting a prop is deleting a folder, and regenerating replaces in place rather than accumulating.

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
1  Concept     prompt + an image pipeline        → concept images
2  References  refinement pipelines (optional)   → reference images
3  Mesh        a mesh pipeline                   → candidates
4  Post        post pipelines, in order          → a better mesh
5  Import      collision, Nanite, LODs, textures → a UStaticMesh
```

**What runs today: Concept, Mesh and Import.** Stage 2 has two halves and only one of them is
outstanding: *choosing* which pictures the generator sees works and is where most of the value is;
*making more pictures to choose from* — background removal, extra angles — has no pipeline shipped
yet. On a provider whose image endpoint already removes backgrounds and draws multiple views, that
half is unnecessary rather than missing. Post is designed, its settings save, and its Run button
says so rather than appearing to work.

### Nothing blocks the editor

Drawing takes fifteen seconds to two minutes and generating takes minutes. Both start and return; a
**Work in progress** strip above the stages lists what is running, for which definition, with a
running clock, and keeps a failed job's reason where it can be read.

This is not a nicety. Drawing used to run on the game thread and pump HTTP inside the wait, which
stopped Unreal redrawing for the whole of it — indistinguishable from a hang, and somebody who sees
nothing happen presses the button again.

### Stale, not discarded

Every stage stores a hash of the inputs it was actually run with, and that hash includes every stage
before it. Edit the prompt and the *mesh* goes stale, not just the concept image — because the mesh
was made from a picture that no longer exists.

**The mesh is kept.** It cost minutes and money, and discarding it to protect a cheap invariant
would throw away the expensive thing. What must not happen is showing it as though it still matched,
and that is what the stage status is for.

### The panel

Four tabs. **Stages** carries a proper multi-line prompt editor and, on each stage that takes one
pipeline, that pipeline's own settings inline — choosing the local image pipeline puts a model, a
size and a step count on the row; choosing the Meshy one replaces them with a model, an aspect ratio
and a pose. Neither list is written in the panel. Both come from the same reflected properties an
agent reads.

**Images** shows one picture large with a filmstrip beneath it, arrow keys to walk it, and the
buttons that decide what the generator is shown. A 168-pixel tile cannot answer the only question
anybody asks of a reference — is this worth reconstructing from — so it is not a grid.

**Mesh** is a viewport built on `FAdvancedPreviewScene`, the same one the Static Mesh editor uses,
so a generated prop is judged under the lighting everything else is judged under. Viewer on the
left, controls on the right.

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
| Image | `Image - Local (this machine's GPU)` | 106s cold, 23s warm, free |
| Image | `Image - Meshy (nano-banana)` | 19s, 3 credits |
| Mesh | `Meshy - Standard (meshy-7)`, `Meshy - Smart Topology (T2)` | wired, priced, unspent |
| Refine | — | |
| Post | — | |

**An image pipeline does its own work, and that is a deliberate exception.** Everywhere else a
pipeline declares and a provider acts. A provider interface can only carry what every provider has
in common, and for image generation that is very nearly nothing — a local diffusion model wants
steps, a size and a seed; a hosted one wants a model name, an aspect ratio and a pose. Routing them
through one call meant those settings were drawn, saved, hashed for staleness, and then dropped on
the floor at the moment they mattered.

A mesh pipeline keeps the declare-and-act split, but it writes its settings into the request through
`Apply` — and it chooses the provider. Without that, the panel could say "Meshy, about 30 credits"
while generation ran on a free local container, or the reverse.

**Meshy ships two rather than one**, and the reason is a trap found on a live account: `ultra_mode`,
`image_enhancement` and `auto_size` do not exist in Smart Topology, and sending one is not refused —
it silently switches mode and doubles the bill. One asset with half its settings greyed out would
leave that one careless click away.

Post-processing is an **array**, because anything that takes a mesh and returns one belongs there,
and a prop is often assembled from several vendors: the shape from one, the textures from another,
the topology from a third.

## Every generated picture becomes an asset

Images are ingested as real `UTexture2D`s in `/Game/_Generated/Mesh/Images/<Definition>/`, appended
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

- **No rigging, and no skeletal meshes.** A generated character comes back as one watertight
  surface with no skeleton and no part decomposition. Useful as a statue or a background figure;
  not an animated character. Pretending otherwise would mean a half-working skeletal path that is
  wrong for the props this is actually for.
- **No level placement.** Dragging a finished mesh into a level is a person's job.

## Using it

1. Enable a provider plugin, and set it in **Project Settings > Plugins > MeshForge**.
2. Right-click in the Content Browser: **Automation Forge > MeshForge > Mesh Definition**.
3. Fill in a prompt, a reference image, or both. Check the provider's capabilities first — some
   generate from text, some only from an image.
4. Generate. The first successful take is imported and finished automatically.

Everything one prop needs — the mesh, its materials, its textures — lands in a single folder named
after it. That is deliberate: deleting a prop is deleting a folder, and it is the only layout in
which regenerating *replaces* the old assets instead of piling new ones beside them.

You can also finish a glTF you already have, with no provider and no GPU — useful for a mesh from a
web tool, a scan, or a colleague:

```
MeshForge.ImportFile C:/path/to/thing.glb SM_Thing
```

Console commands:

```
MeshForge.ListProviders     what is installed, and whether each can generate right now
MeshForge.TestConnection    check one provider
```

## Settings

Split three ways, by who owns the answer:

| Where | What |
|---|---|
| **Project Settings > Plugins > MeshForge** | Output paths, the default provider, quality and finish defaults. Committed; the team shares them. |
| **Editor Preferences > Automation Forge > MeshForge** | Which provider *I* use, and the field that accepts my key. Never committed. |
| The OS credential vault | Secrets. Never in any `.ini`. |

## For agents

[MeshForgeToolset](../MeshForgeToolset) exposes the pipeline through the Unreal toolset registry:
list providers, author definitions, list and set pipelines, draw concept images, list and choose
pictures, estimate, generate, import, re-finish, and list what is running. It is a thin adapter —
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

## Requires

Unreal Engine 5.8, with Interchange's glTF support (on by default). Editor-only; nothing here
ships in a packaged game.

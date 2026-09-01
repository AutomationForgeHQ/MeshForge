// One mesh: what to ask for, what came back, and what it became.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MeshForgeTypes.h"
#include "MeshForgePipeline.h"
#include "MeshImagePipeline.h"
#include "MeshPostPipeline.h"
#include "MeshDef.generated.h"

class UStaticMesh;
class UTexture2D;

/**
 * The editable, regenerable unit of the pipeline - one asset per prop.
 *
 * Edit the prompt or swap the reference image, generate again. Candidates accumulate rather than
 * being replaced, because on providers with no seed a take that is discarded can never be
 * recreated; the MeshId is the only route back to it.
 *
 * This asset deliberately stops at an imported UStaticMesh with its materials, textures, collision
 * and lightmap UVs. Placing it in a level, making a Blueprint of it, or wiring it to gameplay
 * belongs to whoever is building the level, not here.
 *
 * **Static meshes only, deliberately.** A generated character comes back as one watertight surface
 * with no skeleton and no part decomposition, so rigging it is a separate problem with separate
 * tools. Pretending otherwise here would mean a half-working skeletal path that is wrong for the
 * props this plugin is actually for.
 *
 * **The declaration order below is the order the details panel draws.** Unreal sorts categories by
 * where each one first appears, so a property put in a tidy-looking place lands its whole category
 * somewhere surprising - which is how this asset once showed Finish between the concept and the
 * references, and the mesh stage after post-processing. The numbers in the category names are
 * there so the panel matches the Stages tab beside it, and the sections below run in that order.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Mesh Definition"))
class MESHFORGE_API UMeshDef : public UDataAsset
{
	GENERATED_BODY()

public:

	// ---------------------------------------------------------------------------------------------
	// The brief
	// ---------------------------------------------------------------------------------------------

	/**
	 * What the object is.
	 *
	 * Feeds the concept stage, and on a provider that generates from text it is the whole brief.
	 * First because it is the field with the most effect on the result and the one people rewrite
	 * most often - the panel gives it its own box above the stages for the same reason.
	 *
	 * Describe the object, not the picture: "a dented steel ammunition crate with rope handles" is a
	 * prop, "a photo of a crate on a white background, studio lighting" is a photograph of one, and
	 * the second phrasing puts the studio in the mesh.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prompt", meta = (MultiLine = true))
	FString Prompt;

	// ---------------------------------------------------------------------------------------------
	// Stage 1 - concept image
	// ---------------------------------------------------------------------------------------------

	/**
	 * Which building block draws the reference, and how it is configured.
	 *
	 * The single largest quality lever in the whole pipeline, and usually the cheapest. The same
	 * prompt through a local SDXL and through a hosted model produced a smooth white box and a
	 * machine with legible plate lettering, gauge faces and levelling feet. The mesh can only ever
	 * be as good as what it was shown.
	 *
	 * Empty is perfectly normal - it means somebody is supplying their own pictures instead.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "1 Concept")
	TObjectPtr<UMeshImagePipeline> ConceptPipeline;

	/**
	 * Start the concept stage: draw a reference image and ingest it into the project.
	 *
	 * **Returns immediately.** Drawing takes fifteen seconds to two minutes and runs in the
	 * background; watch the stage's status, or the job list, for when it lands. It used to run
	 * inline and froze the editor for the whole of it, which reads as a hang rather than as work.
	 *
	 * Explicit rather than the implicit draw that used to happen inside generation, and the
	 * difference matters: that one used the picture for one submission and threw it away, so nobody
	 * could look at what the mesh had been shown, keep a good one, or try three and pick. Every
	 * image this produces becomes a real texture asset.
	 *
	 * Appends to ConceptImages and makes the newest the main image. Marks the stages after it stale
	 * without discarding what they made.
	 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "1 Concept", meta = (DisplayName = "Draw"))
	void DrawConceptImage();

	/**
	 * Take what a concept draw produced and write it into the project. Game thread only.
	 *
	 * Called by the subsystem when the background half finishes. Separate from starting the draw
	 * because creating assets, dirtying packages and opening a transaction are all things that may
	 * only happen on the game thread - and because the definition, not the subsystem, is what knows
	 * where its own pictures go.
	 */
	bool FinishConceptDraw(const FMeshImageDrawResult& Drawn, FString& OutError);

	/**
	 * Every image this stage has drawn, oldest first, ingested as project assets.
	 *
	 * Kept rather than replaced for the same reason mesh candidates are: on a provider with no
	 * seed, a picture discarded is a picture that cannot be drawn again.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "1 Concept")
	TArray<TSoftObjectPtr<UTexture2D>> ConceptImages;

	/**
	 * Pictures brought in by hand rather than drawn here.
	 *
	 * A photograph, a screenshot, something from an art bible, a render from another tool. They sit
	 * in the same gallery as the generated ones and can be chosen the same way, because from the
	 * mesh model's point of view there is no difference between a picture that was drawn and a
	 * picture that was found.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "1 Concept")
	TArray<TSoftObjectPtr<UTexture2D>> AddedImages;

	/**
	 * Legacy index into ConceptImages. Read only as a fallback when MainImage is unset.
	 *
	 * Superseded by MainImage, which points at a texture rather than at a position - an index
	 * silently means a different picture the moment anything is inserted before it.
	 */
	UPROPERTY()
	int32 SelectedConcept = -1;

	// ---------------------------------------------------------------------------------------------
	// Stage 2 - references
	//
	// Two separate jobs share this stage, and they are worth telling apart.
	//
	// The first is *choosing*: which of the pictures in the gallery the mesh model is actually
	// shown. Every definition does this, and a definition with one good picture does nothing else -
	// the main image goes straight into generation.
	//
	// The second is *making more pictures to choose from*: another angle, a cut-out, a delit copy.
	// Only some definitions need it, and on a provider whose image endpoint already removes
	// backgrounds and draws multiple views it never happens at all.
	// ---------------------------------------------------------------------------------------------

	/**
	 * The one picture every mesh model is shown. Falls back to the legacy fields when unset.
	 *
	 * A texture rather than an index into the gallery, so that generating another concept, deleting
	 * a bad one or dropping in a photograph cannot silently change what the mesh is made from.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "2 References")
	TSoftObjectPtr<UTexture2D> MainImage;

	/**
	 * Up to three more angles of the same subject, for a model that reconstructs from several views.
	 *
	 * **Ignored entirely by a single-view model, and that is not a bug.** TRELLIS.2 takes one
	 * picture; Meshy's multi-image endpoint takes up to four. The mesh stage reads this only where
	 * the chosen provider says it can use it, and says so in the log when it drops them.
	 *
	 * **Worth being sceptical of even where it is supported.** Extra views were tried three ways
	 * and each made the reconstruction measurably worse than the single picture it came from: a
	 * model fuses contradictory evidence rather than averaging it, and puts the same feature on
	 * every face. Use these when the views are a genuine orbit of one object, not when they are
	 * four separate drawings of the same idea.
	 */

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "2 References")
	TArray<TSoftObjectPtr<UTexture2D>> ExtraViews;

	/**
	 * What happens to the picture before reconstruction. Empty passes it through untouched.
	 *
	 * Optional, and on some providers unnecessary rather than unimplemented: Meshy's image endpoint
	 * removes backgrounds and draws multiple views itself, so a definition drawing through it has
	 * nothing left for this to do.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "2 References")
	TArray<TObjectPtr<UMeshForgePipeline>> RefinementPipelines;

	/** What the refinement pipelines produced. Shown in the gallery and choosable like any other. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "2 References")
	TArray<TSoftObjectPtr<UTexture2D>> ReferenceImages;

	/** How many extra views this definition's mesh provider can actually use. Zero for most. */
	UFUNCTION(BlueprintPure, Category = "2 References")
	int32 MaxExtraViews() const;

	/**
	 * The picture the mesh stage will use, resolving the legacy fields behind it.
	 *
	 * MainImage, then SourceImage, then the concept the old index pointed at, then the newest
	 * concept. Resolved on read rather than migrated on load, because rewriting somebody's asset
	 * the first time they open it is a change they did not ask for and cannot see.
	 */
	UFUNCTION(BlueprintPure, Category = "2 References")
	TSoftObjectPtr<UTexture2D> ResolveMainImage() const;

	/** Every picture this definition holds, added and generated, in the order the gallery shows. */
	UFUNCTION(BlueprintPure, Category = "2 References")
	TArray<TSoftObjectPtr<UTexture2D>> GatherImagePool() const;

	// ---------------------------------------------------------------------------------------------
	// Stage 3 - the mesh itself
	// ---------------------------------------------------------------------------------------------

	/**
	 * Which generator makes the shape, and how it is configured.
	 *
	 * Takes precedence over the ProviderId and Control fields in Advanced, which remain for
	 * definitions made before pipelines existed and for the agent tools, where naming a provider
	 * and a quality step is the whole of what an agent wants to say.
	 *
	 * **Ask this for geometry only where a later stage will paint it.** Buying textures from the
	 * geometry vendor and replacing them in post costs twice, and the post pipelines say plainly
	 * whether any of them produces textures.
	 */
	/**
	 * A mesh you already have, used instead of generating one.
	 *
	 * **Setting this switches the whole generation half off.** The concept, reference and mesh
	 * stages have nothing left to do, generating is refused rather than quietly wasted, and the
	 * definition becomes a way of running post-processing over a mesh that already exists - a
	 * modular corridor, a hand-built prop, something a colleague made, output from another tool.
	 *
	 * It is exactly the same shape as adding your own picture instead of drawing one, one stage
	 * further down. From the post stage's point of view there is no difference between a mesh that
	 * was generated and a mesh that was modelled: it gets glTF either way.
	 *
	 * The mesh is written out to glTF when a post step needs it, which is why anything a vendor
	 * cannot read - a Nanite-only source with no fallback, a mesh with no LOD 0 - fails here rather
	 * than halfway through a paid job.
	 *
	 * Clear it to go back to generating.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "3 Mesh")
	TSoftObjectPtr<UStaticMesh> SourceMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "3 Mesh")
	TObjectPtr<UMeshForgePipeline> MeshPipeline;

	/**
	 * How many takes to generate.
	 *
	 * **One by default, and deliberately so.** On a pay-as-you-go provider every variant is billed
	 * the moment it is submitted, kept or discarded - so a default of four charges four times for a
	 * definition somebody made to try a single idea.
	 *
	 * Raise it freely where generation is unmetered: a local runner costs nothing, more takes is
	 * strictly better, and the editor says which case you are in before you spend.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "3 Mesh", meta = (ClampMin = 1, ClampMax = 16))
	int32 Variants = 1;

	/**
	 * Generate, then import the first take that succeeds.
	 *
	 * On a metered provider this is the call that spends. The line above it says which kind you are
	 * pointed at before you press it.
	 */
	UFUNCTION(CallInEditor, Category = "3 Mesh", meta = (DisplayName = "Generate"))
	void GenerateNow();

	/** What generating this would cost and roughly how long, without generating it. */
	UFUNCTION(CallInEditor, Category = "3 Mesh", meta = (DisplayName = "What Would This Cost?"))
	void EstimateNow();

	/**
	 * A reference image already in the project.
	 *
	 * Superseded by MainImage and kept for definitions made before the gallery existed. It still
	 * counts as an added picture and still resolves as the main one when nothing else is chosen.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "3 Mesh|Advanced")
	TSoftObjectPtr<UTexture2D> SourceImage;

	/**
	 * A reference image on disk, for the common case of having just been handed a PNG.
	 *
	 * Read at submit time and not stored, so this is a convenience rather than a record. Where a
	 * definition matters, add the image to the gallery instead - a definition that references a
	 * file on somebody's desktop is not something a team can share.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "3 Mesh|Advanced", meta = (FilePathFilter = "Image files (*.png;*.jpg;*.jpeg;*.webp)|*.png;*.jpg;*.jpeg;*.webp"))
	FString SourceImagePath;

	/** Which provider to use. Falls back to the settings default when unset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "3 Mesh|Advanced",
		meta = (GetOptions = "/Script/MeshForge.MeshForgeSettings.GetProviderOptions"))
	FName ProviderId;

	/** Provider model identifier. Falls back to the provider's default when empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "3 Mesh|Advanced")
	FString ModelId;

	/**
	 * Seed, quality, polygon budget, texture size.
	 *
	 * On a provider that seeds, filling this in is what turns a definition into a complete recipe -
	 * the mesh becomes reproducible from the asset alone and the downloaded file stops being
	 * precious. Providers that cannot honour a field ignore it and say so in the log.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "3 Mesh|Advanced")
	FMeshControl Control;

	// ---------------------------------------------------------------------------------------------
	// Stage 4 - post-processing
	// ---------------------------------------------------------------------------------------------

	/**
	 * Everything that happens to the mesh after it exists, in order. Empty imports it as generated.
	 *
	 * Any step that takes a mesh and returns one belongs here - retexture, retopologise, decimate,
	 * segment, or another generation pass over what came back. An array because a prop is often
	 * assembled from several vendors: the shape from one, the textures from another, the topology
	 * from a third, none of which any single vendor offers.
	 *
	 * **Order changes the result.** Unwrapping before retopology throws the UVs away; baking maps
	 * before decimating bakes detail the decimation then removes. A step that cannot run on what it
	 * was handed fails the stage rather than being skipped, because a chain that quietly drops a
	 * step produces a plausible mesh that is not the one that was asked for.
	 */
	UPROPERTY(EditAnywhere, Instanced, BlueprintReadWrite, Category = "4 Post")
	TArray<TObjectPtr<UMeshPostPipeline>> PostPipelines;

	/**
	 * Run every enabled post step in order, then import what comes out.
	 *
	 * **Returns immediately.** Each step is a job of its own and some of them are hosted services
	 * that take minutes; watch the stage, or the job list. On a metered step this is the call that
	 * spends, and the cost line above says what before you press it.
	 *
	 * Works on whichever mesh this definition would use: the one supplied above, or the chosen take
	 * if it generated one. A definition with neither says so rather than starting.
	 */
	UFUNCTION(CallInEditor, Category = "4 Post", meta = (DisplayName = "Run Post-processing"))
	void RunPostNow();

	// ---------------------------------------------------------------------------------------------
	// Stage 5 - import
	// ---------------------------------------------------------------------------------------------

	/**
	 * How the mesh is finished once it is in the project.
	 *
	 * Changing anything here and re-importing costs nothing - no generation, no money, no waiting on
	 * a GPU. That is the whole reason it is a separate struct from Control.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "5 Import")
	FMeshFinishSettings Finish;

	/**
	 * Re-apply the finish settings to the mesh already imported.
	 *
	 * Free, and the one to reach for first: collision, size, pivot, Nanite and lightmap UVs all
	 * change here with no generation, no download and no provider.
	 */
	UFUNCTION(CallInEditor, Category = "5 Import", meta = (DisplayName = "Re-finish Mesh"))
	void RefinishNow();

	// ---------------------------------------------------------------------------------------------
	// State - written by the pipeline, read by everyone
	// ---------------------------------------------------------------------------------------------

	/**
	 * What each stage has done, and whether its output still matches its inputs.
	 *
	 * Keyed by stage so a stage added later does not shift anybody's saved data. See
	 * FMeshStageState for why the input hash is the whole mechanism.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "6 State")
	TMap<EMeshStage, FMeshStageState> Stages;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "6 State")
	EMeshDefStatus Status = EMeshDefStatus::Draft;

	/** Every take ever generated for this definition. Never pruned automatically. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "6 State")
	TArray<FMeshCandidate> Candidates;

	/** The chosen take. Set by review, or by automatic mode picking the first success. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "6 State")
	FString SelectedMeshId;

	/** Why the last operation failed. Cleared when one succeeds. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "6 State")
	FString LastError;

	/** Batch this definition is currently part of, if any. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "6 State")
	FString ActiveBatchId;

	/**
	 * The image actually used for the last generation, where the provider drew one from the prompt.
	 *
	 * Kept because it is the missing half of a text-to-mesh recipe: with an image-only model, the
	 * mesh is reproducible from this picture, and the picture is reproducible from the prompt and
	 * seed only if nothing in the drawing model changed. Recording it makes the second link
	 * unnecessary.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "6 State")
	TSoftObjectPtr<UTexture2D> GeneratedConceptImage;

	/** That stage's state, or an empty one. Never null, so a panel can draw without checking. */
	UFUNCTION(BlueprintCallable, Category = "6 State")
	FMeshStageState GetStageState(EMeshStage Stage) const;

	/**
	 * What this stage's inputs currently hash to.
	 *
	 * Compared against the hash stored on the stage to decide whether it has drifted. Includes the
	 * inputs of every stage before it, so editing the prompt marks the mesh stale rather than only
	 * the concept image.
	 */
	UFUNCTION(BlueprintCallable, Category = "6 State")
	FString ComputeStageHash(EMeshStage Stage) const;

	/**
	 * Recompute every stage's status from its stored hash.
	 *
	 * Marks drifted stages Stale and leaves their outputs alone - a mesh costs minutes and money,
	 * and discarding one because a prompt was edited would throw away the expensive thing to
	 * protect a cheap invariant. What must not happen is showing it as though it still matched.
	 */
	UFUNCTION(BlueprintCallable, Category = "6 State")
	void RefreshStaleness();

	/** True when jobs are in flight and a second submit would be a duplicate charge. */
	UFUNCTION(BlueprintPure, Category = "6 State")
	bool IsBusy() const;

	/** Candidates that finished successfully and could be downloaded. */
	UFUNCTION(BlueprintPure, Category = "6 State")
	int32 CountUsableCandidates() const;

	/** True when there is enough to submit: a prompt, an image, or both. */
	UFUNCTION(BlueprintPure, Category = "6 State")
	bool HasInput() const;

	// ---------------------------------------------------------------------------------------------
	// Result
	// ---------------------------------------------------------------------------------------------

	/** The imported mesh. This is what the plugin exists to produce. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "7 Result")
	TSoftObjectPtr<UStaticMesh> ImportedMesh;

	/** What the last import produced and how well, including anything that went quietly wrong. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "7 Result")
	FMeshImportOutcome LastImport;

	/**
	 * Write this definition's mesh out as a .glb, beside the project.
	 *
	 * Free, and useful well beyond this plugin: it is how a mesh leaves Unreal for Blender, for a
	 * vendor, or for anybody without the project. Post-processing does this for itself when it needs
	 * to - this is the same call with a button on it, so the file can be looked at before a paid
	 * step is asked to read it.
	 *
	 * Exports the supplied source mesh where there is one, otherwise the imported result. Geometry
	 * only: the materials a texturing service is about to replace are not worth the bake.
	 */
	UFUNCTION(CallInEditor, Category = "7 Result", meta = (DisplayName = "Export glTF"))
	void ExportGlbNow();

	// ---------------------------------------------------------------------------------------------
	// Queries and plumbing - nothing reflected, so nothing here reaches the details panel
	// ---------------------------------------------------------------------------------------------

	/** The candidate matching SelectedMeshId, or null when nothing is chosen. */
	const FMeshCandidate* FindSelectedCandidate() const;

	/** The candidate with this mesh id, or null. */
	const FMeshCandidate* FindCandidate(const FString& MeshId) const;
	FMeshCandidate* FindCandidateMutable(const FString& MeshId);

	/** The candidate for a job that is still in flight, or null. */
	FMeshCandidate* FindCandidateByJobMutable(const FString& JobId);

	/** Apply an authoring spec, leaving pipeline state untouched. */
	void ApplySpec(const FMeshDefSpec& Spec);

	/**
	 * Fill in the project's defaults for whatever is still unset - provider, model, finish settings.
	 * Never overwrites a field that has been chosen.
	 *
	 * Called on every newly created definition, whether an agent made it or a person made one in the
	 * Content Browser, so both arrive configured the same way. A definition created by hand that
	 * skipped this looked identical and generated against nothing.
	 */
	void ApplyProjectDefaults();

	/** Move to a new status, recording an error when moving to Failed. */
	void SetStatus(EMeshDefStatus NewStatus, const FString& Error = FString());
};

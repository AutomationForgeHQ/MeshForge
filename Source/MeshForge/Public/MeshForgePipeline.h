// A pipeline: one provider's working parts, as typed settings somebody can see and an agent can read.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MeshForgeTypes.h"
#include "IMeshProvider.h"
#include "MeshForgePipeline.generated.h"

/** Where in the flow a pipeline can be used. A stage only offers pipelines of its own kind. */
UENUM(BlueprintType)
enum class EMeshPipelineKind : uint8
{
	/** Text to picture. Produces one or more images and consumes nothing but a prompt. */
	Image   UMETA(DisplayName = "Image generation"),

	/** Picture to pictures: background removal, delighting, other views. */
	Refine  UMETA(DisplayName = "Image refinement"),

	/** Pictures to a mesh. */
	Mesh    UMETA(DisplayName = "Mesh generation"),

	/** Mesh to mesh - retexture, retopologise, decimate, segment. Anything after a mesh exists. */
	Post    UMETA(DisplayName = "Post-processing"),
};

/**
 * What a pipeline needs and what it hands on.
 *
 * Declared so a stage can refuse an impossible chain before it is run rather than after: a
 * retexture placed before anything has made a mesh, or a mesh pipeline handed no picture. A chain
 * that fails at step four having already paid for steps one to three is the expensive kind of
 * wrong.
 */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshPipelineIO
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "IO")
	bool bNeedsPrompt = false;

	UPROPERTY(BlueprintReadOnly, Category = "IO")
	bool bNeedsImages = false;

	UPROPERTY(BlueprintReadOnly, Category = "IO")
	bool bNeedsMesh = false;

	UPROPERTY(BlueprintReadOnly, Category = "IO")
	bool bProducesImages = false;

	UPROPERTY(BlueprintReadOnly, Category = "IO")
	bool bProducesMesh = false;

	/**
	 * This pipeline paints the mesh.
	 *
	 * Read by the mesh stage, which can then ask its own provider for geometry only - the whole
	 * reason a mixed chain saves money rather than paying twice.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "IO")
	bool bProducesTextures = false;
};

/**
 * One provider's working parts, exposed as typed properties.
 *
 * **A C++ class, written by us, one per provider or per provider flow.** Meshy can ship a single
 * pipeline covering both of its modes, or two - `Meshy Standard` and `Meshy Smart Topology` - where
 * the modes differ enough that one asset would be a maze of settings that only apply half the time.
 * That is a judgement about the vendor, made once, by whoever writes the subclass.
 *
 * **Typed properties rather than a bag of strings, and that is the point.** A `TMap<FString,
 * FString>` cannot show a slider, cannot enumerate a vendor's four topology modes, cannot grey out
 * an option that does not apply to the current model, and cannot carry a tooltip. Unreal's
 * reflection does all of that for free the moment a setting is a real `UPROPERTY` - which means the
 * human surface and the agent surface can come from the same declaration instead of drifting apart.
 *
 * **Instances are assets.** Somebody configures `Meshy - hero props, 4K, no remesh` once and points
 * forty definitions at it; changing it changes all forty. That is worth more than per-definition
 * settings, and it is why this is a `UDataAsset` rather than a struct.
 *
 * A pipeline does not run anything. It says what should happen, and the subsystem asks the named
 * provider to do it - the same separation the rest of this plugin keeps, so that a pipeline asset
 * survives a provider being rewritten.
 */
UCLASS(Abstract, BlueprintType, EditInlineNew, meta = (DisplayName = "MeshForge Pipeline"))
class MESHFORGE_API UMeshForgePipeline : public UDataAsset
{
	GENERATED_BODY()

public:

	/** What this instance is for, in a sentence. Shown wherever one is picked. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline", meta = (MultiLine = true))
	FString Description;

	/**
	 * Off keeps a pipeline in a chain without running it.
	 *
	 * Worth having rather than deleting, because comparing "with delighting" against "without" is
	 * the common case and rebuilding the step loses its settings.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline")
	bool bEnabled = true;

	// --- what the subclass declares ---------------------------------------------------------------

	/** Which stage this belongs to. */
	UFUNCTION(BlueprintCallable, Category = "Pipeline")
	virtual EMeshPipelineKind GetKind() const PURE_VIRTUAL(UMeshForgePipeline::GetKind, return EMeshPipelineKind::Mesh;);

	/** The provider that carries this out. */
	UFUNCTION(BlueprintCallable, Category = "Pipeline")
	virtual FName GetProviderId() const PURE_VIRTUAL(UMeshForgePipeline::GetProviderId, return NAME_None;);

	/** What it needs and what it hands on. See FMeshPipelineIO. */
	UFUNCTION(BlueprintCallable, Category = "Pipeline")
	virtual FMeshPipelineIO GetIO() const PURE_VIRTUAL(UMeshForgePipeline::GetIO, return FMeshPipelineIO(););

	/**
	 * What one run of this will cost, as a sentence. Empty where it is free.
	 *
	 * Asked before the run, not after. A chain that spends on four providers should be able to say
	 * what it is about to spend before anybody presses the button.
	 */
	UFUNCTION(BlueprintCallable, Category = "Pipeline")
	virtual FString DescribeCost() const;

	/**
	 * Why this pipeline cannot run as configured, or empty when it can.
	 *
	 * Settings that contradict each other are a vendor-specific matter and belong to whoever wrote
	 * the subclass. On at least one provider an option that does not belong to the current mode is
	 * not refused - it silently changes the mode and doubles the bill - so catching that here is
	 * the difference between a warning and an invoice.
	 */
	UFUNCTION(BlueprintCallable, Category = "Pipeline")
	virtual FString Validate() const;

	/**
	 * Write this pipeline's settings into the request the provider will read.
	 *
	 * **Without this a pipeline is decoration.** Its properties are drawn by the panel, saved on the
	 * definition and hashed into staleness, and then nothing puts them anywhere the provider looks -
	 * which is exactly the bug the image pipelines had, and it is invisible: the mesh comes back,
	 * just not the one that was asked for.
	 *
	 * **Takes the whole request, not just the control.** A pipeline chooses the model - on Meshy the
	 * pipeline *is* the model, two `ai_model` values with different prices and different accepted
	 * fields - and it can also contribute pictures, which a string map cannot carry. It used to take
	 * a control and a model id, and a texture override had nowhere to put its images.
	 *
	 * Default does nothing, which is right for a pipeline whose settings are all in its own hands.
	 */
	virtual void Apply(FMeshSubmitRequest& Request) const {}

	/**
	 * How many *extra* views this pipeline's model will actually read. -1 means "ask the provider".
	 *
	 * **Declared by the pipeline because the pipeline is what knows the model.** A provider-wide
	 * answer is a guess the moment one vendor's models differ from each other, and the cost of that
	 * guess is paid in the worst possible way: somebody generates four views, is told four will be
	 * used, and gets a reconstruction from one - then spends the afternoon wondering why the back of
	 * the object is wrong. Dropping a picture silently is not a small bug on a metered service.
	 *
	 * Zero is a real answer and means "this model reconstructs from one picture". Anything shown to
	 * a person must say so before they draw four.
	 */
	UFUNCTION(BlueprintCallable, Category = "Pipeline")
	virtual int32 GetMaxExtraViews() const { return -1; }

	/**
	 * A stable string describing what this pipeline would do.
	 *
	 * Feeds the definition's stage hash, so editing a pipeline marks every definition that uses it
	 * stale rather than leaving them claiming an output it no longer produces.
	 *
	 * The default walks this object's reflected properties, so a subclass gets a correct signature
	 * without writing one - and cannot forget to update it when it gains a setting.
	 */
	UFUNCTION(BlueprintCallable, Category = "Pipeline")
	virtual FString Signature() const;

	/**
	 * This pipeline's settings as the flat option list agents read.
	 *
	 * Derived from the same reflected properties the details panel draws, so the two surfaces
	 * cannot disagree. A setting added to a subclass appears in both, or in neither.
	 */
	UFUNCTION(BlueprintCallable, Category = "Pipeline")
	TArray<FMeshProviderOption> DescribeOptions() const;
};

// A post pipeline: a mesh goes in, a mesh comes out, and like the image pipelines it does its own work.

#pragma once

#include "CoreMinimal.h"
#include "MeshForgePipeline.h"
#include "MeshPostPipeline.generated.h"

class UStaticMesh;
class USkeletalMesh;

UENUM(BlueprintType)
enum class EMeshPostInputSource : uint8
{
	PreviousStep UMETA(DisplayName="Previous step output (original for first step)"),
	OriginalMesh UMETA(DisplayName="Original mesh"),
	SelectedMesh UMETA(DisplayName="Selected mesh / saved output")
};

/**
 * The mesh a post step is asked to work on, and everything known about where it came from.
 *
 * Deliberately not the definition. A post pipeline never learns what a MeshDef is, for the same
 * reason an image pipeline does not: the moment it can read one it starts making decisions that
 * belong to the stage above it, and it stops being usable from anywhere else.
 */
struct MESHFORGE_API FMeshPostJob
{
	/**
	 * The mesh itself, as glTF binary. Native asset-only steps may omit these bytes.
	 *
	 * Bytes rather than a path or a UStaticMesh, and the reason is the same one IMeshProvider gives
	 * for images: the vendors want it three different ways - a public URL, a base64 data URI, a
	 * multipart upload - and all three are reachable from bytes. A UStaticMesh is not, from a
	 * worker thread, and this runs on one.
	 */
	TArray<uint8> MeshGlb;

	/** A name for the thing, for a vendor that labels its tasks and for the log. */
	FString Name;

	/** What the object is, for a step that wants the brief as well as the geometry. */
	FString Prompt;

	/**
	 * The provider that made this mesh, where one did. None for a mesh somebody modelled.
	 *
	 * **This is what makes retexturing a generated take free of an upload.** A vendor that still
	 * holds the task can be pointed at its own id instead of being handed a hundred megabytes back;
	 * a vendor that has never seen the mesh gets the bytes. Both cases have to work, because the
	 * whole point of a post stage is that it also runs on meshes this plugin did not generate.
	 */
	FName SourceProviderId;

	/** That provider's handle for the mesh. Empty unless SourceProviderId is set. */
	FString SourceTaskId;

	/**
	 * Pictures the definition holds, main view first.
	 *
	 * Offered rather than required. A step that paints from its own chosen images reads its own
	 * settings instead; these are for a step that wants what the mesh was made from.
	 */
	TArray<TArray<uint8>> InputImages;
};

/** What one post step produced, or why it produced nothing. */
struct MESHFORGE_API FMeshPostResult
{
	/** Frozen provenance for this individual step, captured from the worker copy. */
	FString PipelineClass;
	FString PipelineSignature;
	FName PipelineProvider;
	FString InputAssetPath;
	FString VendorTaskId;
	FGuid StepId;
	bool bUsesPreviousOutput = true;
	bool bNativeOutput = false;
	/** The processed mesh, as glTF binary. This is what the next step in the chain is handed. */
	TArray<uint8> MeshGlb;

	/**
	 * The vendor's handle for the result, where it has one.
	 *
	 * Recorded rather than discarded because it is the cheap route back: a finished task can be
	 * re-fetched by id alone for a few days, which is how a paid take was recovered once already.
	 * It is also what the *next* step points at instead of uploading.
	 */
	FString TaskId;

	/** What the vendor says it charged. -1 where it does not say. */
	int32 ConsumedCredits = -1;

	/** Wall clock this step spent. */
	float Seconds = 0.f;

	/** One line for the ledger: what this step actually did. */
	FString Summary;

	/** A sentence for a person. Empty on success. */
	FString Error;

	/**
	 * The mesh is positioned relative to something else and must import exactly where it is.
	 *
	 * Set by a step that fits a mesh to a character. The import stage then keeps the file's
	 * origin and leaves the size alone whatever the definition's finish says, because a finish
	 * that re-centres props is right for every prop and wrong for every garment.
	 */
	bool bKeepPlacement = false;

	bool IsOk() const { return Error.IsEmpty() && (bNativeOutput || MeshGlb.Num() > 0); }
};

/**
 * A pipeline that changes a mesh: retexture, retopologise, decimate, segment.
 *
 * **It carries out its own work, which breaks the "a pipeline only declares" rule on purpose.**
 * The same argument the image pipelines make applies here and is if anything stronger: a provider
 * interface can only carry what every provider has in common, and across a hosted retexture
 * service, a local Blender retopology script and an in-engine decimation there is nothing in
 * common at all. IMeshProvider is an interface for *generating* meshes, and forcing a retexture
 * through SubmitJob would mean a request struct with an image half nobody fills and a mesh half
 * every generator ignores.
 *
 * Run is called on a worker thread and may block for as long as it needs. It must not touch an
 * asset, a package or a widget - the stage above brings the result back to the game thread before
 * anything of that kind happens.
 */
UCLASS(Abstract, BlueprintType, EditInlineNew, meta = (DisplayName = "Post Pipeline"))
class MESHFORGE_API UMeshPostPipeline : public UMeshForgePipeline
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input")
	EMeshPostInputSource InputSource = EMeshPostInputSource::PreviousStep;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input", meta=(EditCondition="InputSource == EMeshPostInputSource::SelectedMesh", EditConditionHides))
	TSoftObjectPtr<UStaticMesh> InputMesh;

	/** Persistent producer identity; output links survive reordered or newly added steps. */
	UPROPERTY() FGuid StepId;

	/** Native skeletal output is finalized on the game thread after the preceding static output imports. */
	virtual bool ProducesSkeletalMesh() const { return false; }
	virtual USkeletalMesh* CreateSkeletalOutput(UStaticMesh* Input, const FString& AssetPath, FString& Error) const
	{
		Error = TEXT("This step does not create skeletal meshes.");
		return nullptr;
	}

	/** Fixed by the class, so a post pipeline cannot be offered anywhere but the post stage. */
	virtual EMeshPipelineKind GetKind() const override final { return EMeshPipelineKind::Post; }

	virtual FMeshPipelineIO GetIO() const override
	{
		FMeshPipelineIO IO;
		IO.bNeedsMesh    = true;
		IO.bProducesMesh = true;
		return IO;
	}

	/**
	 * Read anything that can only be read on the game thread, and keep it. Called before Run.
	 *
	 * **This exists because a pipeline's settings can point at assets and Run cannot touch one.**
	 * A retexture that paints from a picture holds a TSoftObjectPtr to a texture; loading it and
	 * reading its pixels is game-thread work, and doing it inside Run would be a race that shows up
	 * as a corrupt upload rather than as a crash.
	 *
	 * Called on the *copy* the worker will use, so whatever it caches belongs to that run and cannot
	 * be changed underneath it. False with a sentence stops the chain before anything is spent.
	 */
	virtual bool Prepare(FString& OutError) { return true; }

	/**
	 * Do the work. Blocking, on a worker thread.
	 *
	 * Writes the processed mesh into OutResult, or a sentence into its Error. Returning neither is
	 * treated as a failure by the stage, because a step that quietly produced nothing and a step
	 * that quietly succeeded look identical from outside and only one of them is safe to chain.
	 */
	virtual void Run(const FMeshPostJob& Job, FMeshPostResult& OutResult) const
		PURE_VIRTUAL(UMeshPostPipeline::Run, OutResult.Error = TEXT("This post pipeline does nothing."););
};

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
	PreviousStep UMETA(DisplayName="Step above, latest run (the Mesh stage's selected take, for the first step)"),
	OriginalMesh UMETA(DisplayName="Mesh stage's selected take (or Source Mesh)"),
	SelectedMesh UMETA(DisplayName="Selected mesh / saved output"),

	/**
	 * A chosen step, and a chosen run of it - for trying a step with different settings and carrying on
	 * from the run that came out best rather than from whichever ran last. Added last so saved
	 * definitions keep their values.
	 */
	StepOutput   UMETA(DisplayName="A step's output - choose the step and the run")
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
	 * The last vendor task anywhere upstream, however many local steps have run since.
	 *
	 * **Different from SourceProviderId, and the difference is the bug it fixes.** Those two are
	 * overwritten by every step, so they answer "what did the step just before me do" - after a
	 * garment fit they say `GarmentFit` and an empty id, and a shirt genuinely generated on Tripo
	 * reports that it did not come from Tripo. These two are only ever written by a step that
	 * actually returns a vendor handle, so they keep answering "what made this, originally".
	 */
	FName OriginProviderId;

	FString OriginTaskId;

	/**
	 * The vendor's copy of this mesh is still the mesh we are holding.
	 *
	 * True until a step that changes geometry runs (see `FMeshPipelineIO::bChangesGeometry`). While
	 * it is true, a same-vendor step may point at the task id and skip the upload entirely. Once it
	 * is false the id still identifies where the mesh *came from* and is still worth recording -
	 * but it is no longer a usable shortcut, and using it would return the vendor's mesh instead of
	 * ours.
	 */
	bool bVendorCopyIsCurrent = false;

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

	/** Which earlier result in this run the input came from, when bUsesPreviousOutput. -1 for the one just before. */
	int32 FreshFromResult = INDEX_NONE;

	/** The generated take this step started from, by MeshId, where it started from one. What the record names. */
	FString StartTakeId;
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

	/**
	 * The lineage this result carries forward. Set by the chain, not by the step.
	 *
	 * Here as well as on the job because a chain may reach *back* to an earlier step's output -
	 * "Starts from: a step's output" - and that mesh's history is the one that step had, not the
	 * one the chain has reached since.
	 */
	FName OriginProviderId;
	FString OriginTaskId;
	bool bVendorCopyIsCurrent = false;

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

	/**
	 * A static mesh asset the step already made and saved, recorded as its output as it is - no glTF, no
	 * import. Set by an interactive step that edits a mesh inside the editor; empty for everything else.
	 */
	FSoftObjectPath OutputStaticMesh;

	bool IsOk() const { return Error.IsEmpty() && (bNativeOutput || MeshGlb.Num() > 0 || OutputStaticMesh.IsValid()); }
};

/** Where an interactive step stands. See UMeshPostPipeline::PollInteractive. */
UENUM(BlueprintType)
enum class EMeshInteractiveState : uint8
{
	Waiting,
	Done,
	Failed,
};

/** One run of an interactive step: its folder, and the input already written into it. */
struct MESHFORGE_API FMeshPostInteractiveSession
{
	FGuid Id;

	/** Absolute folder holding everything this session reads and writes. */
	FString Directory;

	/** Absolute path of the input, as glTF binary. */
	FString InputGlb;

	/** The input asset, where there is one. Empty for a generated take read from the vault. */
	FString InputAssetPath;

	FString DefinitionName;
};

/** What a native step is told about where its output goes. See UMeshPostPipeline::CreateNativeOutput. */
struct MESHFORGE_API FMeshPostNativeContext
{
	/** A full object path in the post take folder: "/Game/.../Post/<take>/SK_Name". */
	FString AssetPath;

	FString DefinitionName;
	FString DefinitionPath;
	FString Prompt;

	/** The recorded input of the output this step is working on, where there is one. */
	FSoftObjectPath InputMadeFrom;

	/** Written by the step: one line saying what it did. */
	FString Summary;
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

	/**
	 * The asset this step starts from when Input Source is Selected. A static mesh for a geometry step;
	 * a step that makes an asset from a skeletal mesh takes one of those. Only the path is stored, so
	 * definitions saved when this could only hold a static mesh load unchanged.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input", meta=(EditCondition="InputSource == EMeshPostInputSource::SelectedMesh", EditConditionHides,
		AllowedClasses="/Script/Engine.StaticMesh,/Script/Engine.SkeletalMesh"))
	TSoftObjectPtr<UObject> InputMesh;

	/**
	 * The step whose output this one starts from, when Input Source is "A step's output". Its StepId, or
	 * MeshStageSourceId for the Mesh stage - whose takes are then the runs.
	 *
	 * Picked in the stage panel, where these two fields are drawn as a step list and a run list. Kept
	 * reflected so an agent sets them the same way: both ids come from List Post Inputs.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input", meta=(EditCondition="InputSource == EMeshPostInputSource::StepOutput", EditConditionHides, SignatureOmitsDefault))
	FGuid InputStepId;

	/**
	 * Which run of that step, by its take id. Empty means its latest run - in a chain, this chain's run of it.
	 * For the Mesh stage, a generated take's MeshId; empty means the take selected in the Takes tab.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input", meta=(EditCondition="InputSource == EMeshPostInputSource::StepOutput", EditConditionHides, SignatureOmitsDefault))
	FString InputTakeId;

	/** Persistent producer identity; output links survive reordered or newly added steps. */
	UPROPERTY() FGuid StepId;

	/**
	 * The InputStepId that names the Mesh stage rather than a post step.
	 *
	 * **The Mesh stage is where every chain's first mesh comes from, and it was not in the list.** "A
	 * step's output" offered post steps only, so the first step - the one most likely to want a
	 * particular take - was told there was nothing to start from. Fixed, never generated: a StepId is a
	 * random guid, so this cannot collide with one, and a workflow copies it as it copies any other id.
	 */
	static const FGuid MeshStageSourceId;

	/** Starts from a take of the Mesh stage, chosen through "A step's output". */
	bool StartsFromMeshStage() const
	{
		return InputSource == EMeshPostInputSource::StepOutput && InputStepId == MeshStageSourceId;
	}

	/** Native skeletal output is finalized on the game thread after the preceding static output imports. */
	/**
	 * May this step point the vendor at its own copy instead of uploading the mesh?
	 *
	 * **One answer, in one place, because the two retexture steps disagreed.** Meshy asked "is the
	 * step before me Meshy" and Tripo asked the same and then refused outright rather than
	 * uploading; neither noticed that a local step in between had invalidated the answer, and
	 * neither noticed that an id can be perfectly valid lineage and a dead handle.
	 *
	 * Three things have to hold: the mesh came from *this* vendor, there is a handle for it, and
	 * nothing has moved a vertex since the vendor last saw it. Anything else means upload - which
	 * is always correct, merely slower, and is what makes a step work on a mesh this plugin never
	 * generated.
	 */
	bool CanUseVendorTask(const FMeshPostJob& Job) const
	{
		return Job.bVendorCopyIsCurrent
			&& !Job.OriginTaskId.IsEmpty()
			&& Job.OriginProviderId == GetProviderId();
	}

	virtual bool ProducesSkeletalMesh() const { return false; }
	virtual USkeletalMesh* CreateSkeletalOutput(UStaticMesh* Input, const FString& AssetPath, FString& Error) const
	{
		Error = TEXT("This step does not create skeletal meshes.");
		return nullptr;
	}

	// --- native steps: assets in, an asset out, on the game thread ---------------------------------
	//
	// A skeletal wardrobe mesh was the first of these, and the shape was written for it alone: a static
	// mesh in, a skeletal mesh out, and nothing allowed after it. An inventory item made from that
	// skeletal mesh is the same kind of work one step further on, so the contract is the general one
	// below and the skeletal pair above is its first implementation. Once a chain reaches a native step,
	// everything after it is native too - they read assets, and nothing turns an asset back into bytes.

	/** True for a step that works on project assets on the game thread instead of on glTF bytes. */
	virtual bool IsNativeStep() const { return ProducesSkeletalMesh(); }

	/**
	 * Why these steps cannot run in this order, or empty when they can. Switched-off steps are skipped.
	 *
	 * The one rule, asked by the chain before it runs and by anything that reorders it: a step that works
	 * on geometry cannot come after one that makes an asset.
	 */
	static FString ChainOrderProblem(TConstArrayView<const UMeshPostPipeline*> Steps);

	/** Whether a native step can work on an asset of this class. */
	virtual bool AcceptsNativeInput(const UClass* InputClass) const;

	/**
	 * Make this step's output from its input. Game thread; the previous step has already imported.
	 *
	 * Context.AssetPath is a suggestion - the post take folder and a name - which a step may ignore
	 * when its output belongs where a game expects it. The default forwards a static mesh to
	 * CreateSkeletalOutput. Context.Summary is what the step says it did, for the take record.
	 */
	virtual UObject* CreateNativeOutput(UObject* Input, FMeshPostNativeContext& Context, FString& Error) const;

	/** Prefix for the suggested asset name. "SK_" for a skeletal mesh; a step making something else says so. */
	virtual FString GetNativeOutputPrefix() const { return TEXT("SK_"); }

	/**
	 * What this step is called on the chain and in its outputs.
	 *
	 * The class's display name for every pipeline written as a class. A step registered by another
	 * plugin shares one class with every other registered step, so it answers with its own name.
	 */
	virtual FText GetStepDisplayName() const { return GetClass()->GetDisplayNameText(); }

	/** False for a class that exists to host something else and must not appear in the step picker itself. */
	virtual bool IsOfferedInPicker() const { return true; }

	/**
	 * A mesh of this step's own to run on in place of its input: an edit of that input kept with the step, as
	 * the Garment Studio keeps its sculpt. Null runs on the input as the chain resolved it, which is what every
	 * other step does.
	 *
	 * SavedInput is the saved asset the chain resolved - a source mesh, a step's recorded output - or null when
	 * the input is a take's file, or when bInputFromThisRun says an earlier step of this same run makes it. Called
	 * on the game thread, on the definition's own step, before anything is exported or copied. Null with OutError
	 * set refuses the run.
	 */
	virtual UStaticMesh* GetEditedInput(UStaticMesh* SavedInput, bool bInputFromThisRun, FString& OutError) const { return nullptr; }

	/**
	 * Forget what belongs to one definition rather than to how things are made - the session it last opened, an
	 * edit of its input - when the step is copied into a workflow or out of one. The base clears the session.
	 */
	virtual void ForgetDefinitionState() { InteractiveSessionDirectory.Reset(); }

	/** Take that state over from the step this one replaces on the same definition, when a workflow is applied. */
	virtual void KeepDefinitionState(const UMeshPostPipeline& Replaced) { InteractiveSessionDirectory = Replaced.InteractiveSessionDirectory; }

	// --- interactive steps: a person finishes them ------------------------------------------------
	//
	// Editing a mesh in Blender is a step like any other - a static mesh in, a static mesh out, an
	// output recorded with this step's id - except that it ends when somebody says so. The chain stops
	// at it, and carries on from the next step when the result comes back.

	/** True for a step that waits for a person rather than running to completion. */
	virtual bool IsInteractiveStep() const { return false; }

	/**
	 * True for a step that answers Send Interactive Step Command. Every interactive step does; a step that runs
	 * to completion but has a window of its own - the Garment Studio - says so here, and gets an empty session.
	 */
	virtual bool TakesCommands() const { return IsInteractiveStep(); }

	/**
	 * Whether a command needs a session the chain started. True for a step whose window only exists while the
	 * chain waits on it; false for one with a window of its own, which answers with or without a session.
	 */
	virtual bool CommandsNeedSession() const { return IsInteractiveStep(); }

	/**
	 * Start the step. Game thread. The input is already written to Session.InputGlb.
	 *
	 * Launch whatever the person works in and return. False with a sentence when it could not start.
	 */
	virtual bool BeginInteractive(const FMeshPostInteractiveSession& Session, FString& OutError) const
	{
		OutError = TEXT("This step is not interactive.");
		return false;
	}

	/** Has the person finished? Polled on the game thread about twice a second while the step waits. */
	virtual EMeshInteractiveState PollInteractive(const FMeshPostInteractiveSession& Session,
		FString& OutResultGlb, FString& OutSummary, FString& OutError) const
	{
		OutError = TEXT("This step is not interactive.");
		return EMeshInteractiveState::Failed;
	}

	/**
	 * Whether the chain writes the input to Session.InputGlb before BeginInteractive. True for a step that
	 * hands the mesh to another program; a step that edits the asset inside the editor has no use for it.
	 */
	virtual bool WantsInteractiveInputGlb() const { return true; }

	/**
	 * For a result that is already a saved static mesh asset rather than a file: its path, read once
	 * PollInteractive has said Done. The chain then records that asset as the step's output without
	 * exporting or importing anything, so an in-editor edit keeps its vertex order, materials and settings.
	 */
	virtual bool GetInteractiveResultAsset(const FMeshPostInteractiveSession& Session, FSoftObjectPath& OutStaticMesh) const
	{
		return false;
	}

	/** Mark a result as taken, so the same result is never imported twice. */
	virtual void ConsumeInteractiveResult(const FMeshPostInteractiveSession& Session) const {}

	/** After the result imported: anything that needs the new asset and the input together. Game thread. */
	virtual void OnInteractiveOutputImported(UStaticMesh* Output, UObject* Input) const {}

	/**
	 * Carry out one command in whatever the step opened, so an agent or a test can work the step without a
	 * mouse or a keyboard. The words are the step's own; "help" should list them. Game thread. Returns false
	 * with a sentence in OutReply when the command was not understood or could not be done.
	 */
	virtual bool HandleInteractiveCommand(const FMeshPostInteractiveSession& Session, const FString& Command,
		FString& OutReply) const
	{
		OutReply = TEXT("This step takes no commands. Finish it where it opened.");
		return false;
	}

	/**
	 * The folder of the last session this step started, kept so a result sent back after the editor
	 * was closed can still be picked up. Written by the chain, not by hand.
	 */
	UPROPERTY()
	FString InteractiveSessionDirectory;

	/** Fixed by the class, so a post pipeline cannot be offered anywhere but the post stage. */
	virtual EMeshPipelineKind GetKind() const override final { return EMeshPipelineKind::Post; }

	virtual FMeshPipelineIO GetIO() const override
	{
		FMeshPipelineIO IO;
		IO.bNeedsMesh    = true;
		IO.bProducesMesh = true;

		// **On by default, and the asymmetry is the reason.** A post step exists to change the mesh;
		// the ones that leave the geometry alone are the exception and say so. Getting this wrong in
		// the safe direction costs an upload that was not strictly needed - slower, still correct.
		// Getting it wrong the other way hands back the vendor's mesh instead of ours and throws a
		// wrap or a sculpt away without a word.
		IO.bChangesGeometry = true;

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

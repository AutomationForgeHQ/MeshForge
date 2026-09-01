// The pipeline: submit, poll, download, import, finish. One place that knows the order.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "MeshForgeTypes.h"
#include "ForgeLibrary.h"
#include "MeshPostPipeline.h"
#include "Containers/Ticker.h"
#include "MeshForgeSubsystem.generated.h"

class IMeshProvider;
class UMeshDef;
class UTexture2D;

/** Fired whenever anything in a batch moves. Batch id, finished, total. */
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnMeshBatchProgress, const FString&, int32, int32);

/** Fired when one definition reaches a terminal state, successfully or not. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnMeshDefFinished, UMeshDef*, bool /*bSuccess*/);

/** Fired when a background job starts, finishes, or is cleared away. */
DECLARE_MULTICAST_DELEGATE(FOnMeshForgeJobsChanged);

/**
 * Something changed a definition from outside the panel that is showing it.
 *
 * Raised by whoever made the change, with the definition it was made to. The open editor listens
 * and repaints - which is the only way one of its tabs can react to an edit made in another, or to
 * an edit an agent made over MCP while somebody had the asset open.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnMeshDefinitionEdited, UMeshDef*);

/**
 * MeshForge's pipeline.
 *
 * Everything that knows the *order* of things lives here: which provider a definition resolves to,
 * how a prompt with no image becomes one, when a finished job is downloaded, and when a downloaded
 * file becomes an asset. Providers know none of that, and the definition asset knows none of it
 * either - it is a record, not a state machine.
 *
 * Polling rather than callbacks between stages, because the providers this was designed against
 * split three ways on how they report progress and the only thing all three do reliably is answer
 * "is it done yet".
 */
UCLASS()
class MESHFORGE_API UMeshForgeSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	static UMeshForgeSubsystem* Get();

	// ---------------------------------------------------------------------------------------------
	// Providers
	// ---------------------------------------------------------------------------------------------

	/** Every registered provider, sorted. Empty is a normal state - MeshForge ships none of its own. */
	UFUNCTION(BlueprintPure, Category = "MeshForge|Providers")
	TArray<FName> GetProviderIds() const;

	/** What a provider can do and whether it is ready. False when there is no such provider. */
	bool DescribeProvider(FName ProviderId, FMeshProviderCaps& OutCaps, bool& bOutAvailable, FString& OutReason) const;

	/**
	 * Which provider a definition will actually use.
	 *
	 * Resolution order is deliberate: the definition's own choice, then this machine's override, then
	 * the project default. The middle one is what lets one person on a team run a local GPU while
	 * the project names a hosted service.
	 */
	TSharedPtr<IMeshProvider> ResolveProvider(const UMeshDef* Def) const;

	/**
	 * The control a definition would actually be generated with, and the model it names.
	 *
	 * The definition's own advanced settings with its mesh pipeline written over them - which is what
	 * the provider will be handed, and therefore the only thing worth pricing or reporting. Asking
	 * the provider what `Def->Control` costs prices a request nobody is going to send: a Smart
	 * Topology pipeline quoted thirty credits while the panel beside it said fifteen, because one of
	 * them had applied the pipeline and the other had not.
	 */
	FMeshControl ResolveControl(const UMeshDef* Def, FString& OutModelId) const;

	/** True when this definition has a picture to generate from, wherever it came from. */
	bool HasReferenceImage(const UMeshDef* Def) const;

	/** Extra views this definition's provider will actually read. Zero on a single-view model. */
	int32 UsableExtraViews(const UMeshDef* Def) const;

	/**
	 * Everything known about one take, ready to be filed in the library.
	 *
	 * Built from the *resolved* control rather than from the definition's current fields, because
	 * the record has to be what was sent - and those two differ the moment a pipeline writes over
	 * the definition's advanced settings.
	 */
	FForgeTakeRecord BuildTakeRecord(const UMeshDef* Def, const FMeshCandidate& Candidate) const;

	// ---------------------------------------------------------------------------------------------
	// Background work
	//
	// Anything that takes longer than a frame runs here rather than on the game thread. Drawing a
	// concept image is fifteen seconds to two minutes; doing it inline froze the editor for the whole
	// of it, which reads as a hang. Nothing in this section blocks.
	// ---------------------------------------------------------------------------------------------

	/**
	 * Start the concept stage for a definition, in the background. Returns immediately.
	 *
	 * An invalid guid with a sentence in OutError means it could not be started at all - no
	 * pipeline, no prompt, or one already running for this definition. A valid guid means it is
	 * running, and whether it *succeeds* is reported later on the stage and in the job list.
	 *
	 * **The pipeline is copied before the work starts.** The background thread reads its settings
	 * while somebody may still be editing them in the panel, and a half-changed model name is worse
	 * than either value - so the run uses exactly what was configured at the moment the button was
	 * pressed.
	 */
	FGuid StartConceptDraw(UMeshDef* Def, FString& OutError);

	/**
	 * One job by id, without building and sorting the whole list.
	 *
	 * GetJobs copies and sorts, which is fine once but not from a widget that reads a clock every
	 * frame - the job strip was doing exactly that, once per running row.
	 */
	UFUNCTION(BlueprintPure, Category = "MeshForge|Jobs")
	bool FindJob(const FGuid& JobId, FMeshForgeJob& OutJob) const;

	/** How many jobs the list holds, running or finished. For a widget asking "is there anything". */
	UFUNCTION(BlueprintPure, Category = "MeshForge|Jobs")
	int32 NumJobs() const { return Jobs.Num(); }

	/** Everything running or recently finished, newest first. */
	UFUNCTION(BlueprintPure, Category = "MeshForge|Jobs")
	TArray<FMeshForgeJob> GetJobs() const;

	/** True when that definition already has this stage in flight. */
	UFUNCTION(BlueprintPure, Category = "MeshForge|Jobs")
	bool IsStageRunning(const UMeshDef* Def, EMeshStage Stage) const;

	/** How long that stage has been running, in seconds. Zero when it is not. */
	UFUNCTION(BlueprintPure, Category = "MeshForge|Jobs")
	float StageElapsed(const UMeshDef* Def, EMeshStage Stage) const;

	/** Forget every finished job. Running ones are left alone. */
	UFUNCTION(BlueprintCallable, Category = "MeshForge|Jobs")
	void ClearFinishedJobs();

	/**
	 * Record that a definition's mesh generation has started, so it shows up like any other job.
	 *
	 * Mesh generation was already asynchronous - it submits and a ticker polls - but it was
	 * *invisible*, which is the same problem from the user's side: a button that returns instantly
	 * and then nothing on screen changes for four minutes.
	 */
	void BeginMeshJob(UMeshDef* Def);

	/**
	 * Start the mesh stage for a definition, bringing its provider up first if it needs it.
	 *
	 * Returns immediately. An invalid guid with a sentence in OutError means it could not be started
	 * at all; a valid one means the provider is being made ready and the submission follows.
	 *
	 * This exists rather than calling GenerateMeshes directly because a local provider's container
	 * may be stopped, and starting it takes minutes. Generation used to refuse outright in that
	 * case - "no runner is answering, start the container" - which asked somebody to do by hand the
	 * one thing the plugin already knows how to do.
	 */
	FGuid StartMeshGeneration(UMeshDef* Def, FString& OutError);

	/**
	 * Run a definition's post-processing chain in the background, then re-import what comes out.
	 *
	 * Returns immediately. An invalid guid with a sentence means it could not be started - no
	 * enabled steps, no mesh to work on, or a step whose settings contradict each other.
	 *
	 * **The mesh it works on is resolved here, on the game thread, before anything starts.** A
	 * supplied source mesh is written out to glTF; a generated take is read from the file the vault
	 * already holds. Both end up as bytes, which is the only thing a worker thread can carry - and
	 * where the take came from a provider, its task id travels with the bytes so a vendor that
	 * still holds it can be pointed at its own copy instead of being handed it back.
	 *
	 * Steps run in order and each is fed the previous one's output. A step that fails stops the
	 * chain: carrying on would import a mesh that is missing a pass somebody paid for, and it would
	 * look exactly like one that is not.
	 */
	FGuid StartPostProcessing(UMeshDef* Def, FString& OutError);

	/**
	 * The mesh a post chain would start from, as glTF bytes, plus where it came from.
	 *
	 * Game thread only - it can load an asset and run the exporter. Returns false with a sentence
	 * saying which of the two routes was missing.
	 */
	bool ResolvePostInput(
		UMeshDef* Def,
		TArray<uint8>& OutGlb,
		FName& OutProviderId,
		FString& OutTaskId,
		FString& OutError) const;

	FOnMeshForgeJobsChanged OnJobsChanged;

	/**
	 * Raise this after editing a definition from a surface that does not own the whole editor.
	 *
	 * A panel that edits the definition it was given can refresh itself; it cannot refresh its
	 * siblings, and the picture a generator will be sent is shown in two tabs at once. Rather than
	 * teach every widget about every other, whoever writes to the asset says so here.
	 */
	FOnMeshDefinitionEdited OnDefinitionEdited;

	// ---------------------------------------------------------------------------------------------
	// Authoring
	// ---------------------------------------------------------------------------------------------

	/**
	 * Create a mesh definition asset, configured and ready to generate.
	 *
	 * Creates and wires; it does not generate. The caller decides when to spend.
	 */
	UMeshDef* CreateMeshDef(const FString& AssetName, const FMeshDefSpec& Spec, FString& OutError);

	// ---------------------------------------------------------------------------------------------
	// Generation
	// ---------------------------------------------------------------------------------------------

	/**
	 * Submit every definition given, as one batch.
	 *
	 * **Check BatchId before believing anything else.** An empty one means nothing was submitted -
	 * no provider, no credential, no input - and the counts below it are then meaningless. That is
	 * the failure mode this struct exists to make visible.
	 */
	FMeshBatchSubmission GenerateMeshes(const TArray<UMeshDef*>& Defs);

	/** One definition, as a batch of one. */
	FMeshBatchSubmission GenerateMesh(UMeshDef* Def);

	/** True while any job in this batch is still running. */
	UFUNCTION(BlueprintPure, Category = "MeshForge|Generation")
	bool IsBatchRunning(const FString& BatchId) const;

	/** Stop polling a batch and mark its unfinished jobs cancelled. Does not refund anything. */
	UFUNCTION(BlueprintCallable, Category = "MeshForge|Generation")
	void CancelBatch(const FString& BatchId);

	// ---------------------------------------------------------------------------------------------
	// Import
	// ---------------------------------------------------------------------------------------------

	/**
	 * Download a candidate if needed, then import and finish it.
	 *
	 * Blocking on the import, asynchronous on the download. An empty MeshId takes the definition's
	 * selected candidate, or its only usable one.
	 */
	void ImportCandidate(UMeshDef* Def, const FString& MeshId, TFunction<void(const FMeshImportOutcome&)> OnDone);

	/**
	 * Re-apply the finish settings to a mesh already imported.
	 *
	 * The cheap loop, and the one worth reaching for first: collision, scale, pivot, Nanite and
	 * lightmap UVs all change here with no generation and no download.
	 */
	UFUNCTION(BlueprintCallable, Category = "MeshForge|Import")
	FMeshImportOutcome RefinishMesh(UMeshDef* Def);

	/**
	 * Import a take out of the Forge library and make it the definition's mesh.
	 *
	 * **Reads the library rather than the definition's candidate list, and the difference is the
	 * whole point.** A candidate is what one generation recorded on the asset; the library holds
	 * every take ever filed - post-processed ones, which are not candidates at all, and takes whose
	 * definition has since been regenerated or duplicated. Going through the candidate list meant
	 * the Takes tab could list four takes sitting on disk and refuse every one of them.
	 *
	 * Free: the artifact is already here, so no provider is involved and nothing is billed. Where
	 * the file has been cleaned away, the error says which provider made it, so it can be fetched
	 * again rather than regenerated.
	 */
	UFUNCTION(BlueprintCallable, Category = "MeshForge|Import")
	FMeshImportOutcome ImportTake(UMeshDef* Def, const FString& TakeId);

	/**
	 * Import a glTF already on disk, through the same finishing the pipeline uses.
	 *
	 * No provider, no generation, no definition. This exists because the finishing half is worth
	 * having on its own: somebody with a mesh from a web UI, a photogrammetry scan or a colleague
	 * gets the same collision, lightmap UVs, Nanite decision, scale and pivot as a generated one,
	 * without owning a GPU or an account.
	 *
	 * It is also how the import path is tested with no runner in the loop.
	 *
	 * @param AbsoluteFilePath A .glb or .gltf on disk.
	 * @param AssetName        Name for the static mesh. Sanitised. Empty takes the file's own name.
	 * @param Finish           How to finish it.
	 */
	UFUNCTION(BlueprintCallable, Category = "MeshForge|Import")
	FMeshImportOutcome ImportMeshFile(
		const FString& AbsoluteFilePath,
		const FString& AssetName,
		const FMeshFinishSettings& Finish);

	// ---------------------------------------------------------------------------------------------
	// Events
	// ---------------------------------------------------------------------------------------------

	FOnMeshBatchProgress OnBatchProgress;
	FOnMeshDefFinished   OnDefFinished;

private:

	/** One definition's place in a running batch. */
	struct FTrackedJob
	{
		TWeakObjectPtr<UMeshDef> Def;
		FName ProviderId;
		FString JobId;
		int32 VariantIndex = 0;
		double StartedAt = 0.0;
		bool bFinished = false;
	};

	struct FBatch
	{
		TArray<FTrackedJob> Jobs;

		/**
		 * Submissions handed to a provider that have not answered yet.
		 *
		 * **A batch is live while this is non-zero, even with no jobs in it.** A job only joins the
		 * list when the provider's submit call comes back, and that is a network round trip - so
		 * between creating the batch and hearing back, `Jobs` is empty. Without this counter the poll
		 * sees an empty batch, concludes it has finished, and deletes it; the reply then arrives,
		 * finds no batch to join, and the definition generates on the provider with nothing left
		 * watching for the result.
		 */
		int32 Submitting = 0;

		/**
		 * Imports started for this batch that have not finished yet.
		 *
		 * The same hole as `Submitting`, at the other end. A job is marked finished the moment the
		 * *provider* is done, but downloading the artifact and importing it happen afterwards - so
		 * without this the batch retires while a mesh is still being written, and a caller that
		 * waited politely for the batch is told the definition has zero triangles moments before it
		 * gains thirty thousand.
		 */
		int32 Importing = 0;

		/**
		 * Definitions whose first success has already been imported in *this* batch.
		 *
		 * Scoped to the batch, and that is the whole point. Four variants of one definition should
		 * produce one mesh, not four - but a *later* generation of the same definition must import,
		 * because regenerating is how somebody changes the mesh. Deciding this on whether the
		 * definition has any imported mesh at all gets the first case right and the second exactly
		 * backwards: the second generation finishes, says Generated, and silently leaves the old
		 * mesh in place.
		 */
		TSet<TWeakObjectPtr<UMeshDef>> Imported;

		double StartedAt = 0.0;
		bool bCancelled = false;

		bool IsLive() const
		{
			// **Importing belongs here and its absence was a real bug.** The counter above was added
			// for exactly this and then only consulted by IsBatchRunning, so Poll retired the batch
			// the moment the provider was done - while the download and import were still running.
			// Measured 2026-08-31: "Batch finished" logged at 21:07:54, the 40 MB glb landed at
			// 21:07:55, and the import completed at 21:11:59. Anything reading the definition in
			// between got the *previous* take's mesh and triangle count.
			return Submitting > 0
				|| Importing > 0
				|| Jobs.ContainsByPredicate([](const FTrackedJob& Job) { return !Job.bFinished; });
		}
	};

	TMap<FString, FBatch> Batches;

	/**
	 * Background jobs, oldest first.
	 *
	 * Only ever touched on the game thread - a job is added there, and the completion hops back
	 * there before it is updated. That is why there is no lock: the background half of a job owns
	 * nothing in here.
	 */
	TArray<FMeshForgeJob> Jobs;

	/**
	 * Bring a finished post chain back into the project. Game thread only.
	 *
	 * Files the result in the library before importing it, for the same reason a generated take is
	 * filed before import: an import can fail and a paid pass must not be lost with it.
	 */
	void FinishPostRun(
		const FGuid& JobId,
		const TArray<uint8>& Glb,
		const TArray<FMeshPostResult>& Steps,
		const FString& Error);

	/** Finish a job and write its result onto the definition. Game thread only. */
	void FinishJob(const FGuid& JobId, bool bSuccess, const FString& Error);

	/**
	 * Close out a definition's mesh job and write the stage state.
	 *
	 * Bound to OnDefFinished rather than called from each terminal branch of the batch machinery.
	 * There are four of those, and a stage left saying "running" forever because somebody added a
	 * fifth is exactly the kind of rot this avoids.
	 */
	void HandleDefFinished(UMeshDef* Def, bool bSuccess);

	/**
	 * Write library records for this definition's failed takes, so the ledger can add up.
	 *
	 * A failed generation has no artifact and may still have been charged. Recording only successes
	 * makes a definition look cheaper than it was, which is the opposite of what a ledger is for.
	 */
	void FileFailedTakes(UMeshDef* Def);

	FTSTicker::FDelegateHandle PollHandle;
	double LastPollAt = 0.0;

	bool Poll(float DeltaTime);
	void FinishJob(const FString& BatchId, FTrackedJob& Job, bool bSuccess, const FString& Error);
	void ReportProgress(const FString& BatchId);

	/**
	 * Turn whatever a definition points at into PNG bytes a provider can be handed.
	 *
	 * Returns false with a reason rather than empty bytes, because "no image" and "an image that
	 * could not be read" want different messages and are otherwise indistinguishable at the call
	 * site.
	 */
	bool ResolveSourceImage(const UMeshDef* Def, TArray<uint8>& OutPng, FString& OutError) const;

	/** Submit one definition's takes. Returns how many jobs actually started. */
	int32 SubmitDef(const FString& BatchId, UMeshDef* Def, TSharedPtr<IMeshProvider> Provider, FString& OutError);
};

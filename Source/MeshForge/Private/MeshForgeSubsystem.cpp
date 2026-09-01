#include "MeshForgeSubsystem.h"

#include "MeshForge.h"
#include "MeshDef.h"
#include "MeshForgeSettings.h"
#include "MeshForgeEditorSettings.h"
#include "MeshImporter.h"
#include "IMeshProvider.h"
#include "MeshImageIngest.h"
#include "ForgeLibrary.h"
#include "MeshImagePipeline.h"
#include "MeshPostPipeline.h"
#include "MeshExporter.h"
#include "Async/Async.h"
#include "Misc/ScopeExit.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "IAssetTools.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "MeshForgeSubsystem"

namespace MeshForgeSubsystemPrivate
{
	/**
	 * Where a candidate is staged before import.
	 *
	 * **Named after the definition and nothing else, deliberately.** Interchange names the folder
	 * it imports into after the source file, so a name carrying the take id would put every
	 * generation of one definition in a different folder - the project filling with
	 * near-identical props, none of them replacing the last. A stable name means re-importing
	 * replaces, which is what regenerating a definition is supposed to do.
	 *
	 * The cost is that two takes cannot sit on disk at once. That is fine: they are
	 * re-downloadable, and the candidate list is the record of them.
	 */
	static FString StageName(const UMeshDef* Def, const TCHAR* Extension)
	{
		return FString::Printf(TEXT("%s.%s"),
			*ObjectTools::SanitizeObjectName(Def->GetName()),
			Extension);
	}

}

void UMeshForgeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// One ticker for every batch, rather than one per job. A hundred definitions submitted together
	// is a normal thing to do here, and a hundred timers is not.
	PollHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UMeshForgeSubsystem::Poll), 0.5f);

	// Our own listener, so the job list and the stage states close out on every path the batch
	// machinery can take rather than only on the ones somebody remembered.
	OnDefFinished.AddUObject(this, &UMeshForgeSubsystem::HandleDefFinished);
}

void UMeshForgeSubsystem::Deinitialize()
{
	if (PollHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);
		PollHandle.Reset();
	}

	Batches.Empty();

	Super::Deinitialize();
}

UMeshForgeSubsystem* UMeshForgeSubsystem::Get()
{
	return GEditor ? GEditor->GetEditorSubsystem<UMeshForgeSubsystem>() : nullptr;
}

TArray<FName> UMeshForgeSubsystem::GetProviderIds() const
{
	const FMeshForgeModule* Module = FMeshForgeModule::GetPtrIfLoaded();
	return Module ? Module->GetProviderIds() : TArray<FName>();
}

bool UMeshForgeSubsystem::DescribeProvider(
	FName ProviderId, FMeshProviderCaps& OutCaps, bool& bOutAvailable, FString& OutReason) const
{
	FMeshForgeModule* Module = FMeshForgeModule::GetPtr();
	TSharedPtr<IMeshProvider> Provider = Module ? Module->FindProvider(ProviderId) : nullptr;

	if (!Provider.IsValid())
	{
		return false;
	}

	OutCaps = Provider->GetCaps();
	bOutAvailable = Provider->IsAvailable(OutReason);
	return true;
}

TSharedPtr<IMeshProvider> UMeshForgeSubsystem::ResolveProvider(const UMeshDef* Def) const
{
	FMeshForgeModule* Module = FMeshForgeModule::GetPtr();
	if (Module == nullptr)
	{
		return nullptr;
	}

	// The mesh pipeline first, because it is the thing the panel names and prices.
	//
	// Without this the Stages tab could say "Meshy - Standard, about 30 credits" while generation
	// ran on a local TRELLIS container, or the reverse - which is the same mistake in the direction
	// that costs money. A pipeline naming a provider that is not installed is refused rather than
	// quietly falling through, for the same reason the ProviderId path below is.
	if (Def != nullptr && Def->MeshPipeline != nullptr && Def->MeshPipeline->bEnabled)
	{
		const FName Named = Def->MeshPipeline->GetProviderId();

		if (TSharedPtr<IMeshProvider> Chosen = Module->FindProvider(Named))
		{
			return Chosen;
		}

		UE_LOG(LogMeshForge, Warning,
			TEXT("'%s' uses a pipeline that needs provider '%s', which is not registered."),
			*Def->GetName(), *Named.ToString());
		return nullptr;
	}

	// Then the definition's own choice, then this machine's, then the project's. See the header.
	if (Def != nullptr && !Def->ProviderId.IsNone())
	{
		if (TSharedPtr<IMeshProvider> Chosen = Module->FindProvider(Def->ProviderId))
		{
			return Chosen;
		}

		// Deliberately does not fall through silently. A definition naming a provider that is not
		// installed should say so, not quietly generate on a different one and produce a mesh that
		// does not match the rest of the library.
		UE_LOG(LogMeshForge, Warning,
			TEXT("'%s' names provider '%s', which is not registered."),
			*Def->GetName(), *Def->ProviderId.ToString());
		return nullptr;
	}

	const FName Override = GetDefault<UMeshForgeEditorSettings>()->ProviderOverride;
	if (!Override.IsNone())
	{
		if (TSharedPtr<IMeshProvider> Mine = Module->FindProvider(Override))
		{
			return Mine;
		}
	}

	return Module->FindProvider(GetDefault<UMeshForgeSettings>()->DefaultProviderId);
}

FMeshControl UMeshForgeSubsystem::ResolveControl(const UMeshDef* Def, FString& OutModelId) const
{
	if (Def == nullptr)
	{
		return FMeshControl();
	}

	// Shaped through a throwaway request, because that is what Apply takes now - a pipeline can
	// contribute pictures as well as settings, and pricing only needs the settings half.
	FMeshSubmitRequest Shaped;
	Shaped.Control = Def->Control;
	Shaped.ModelId = Def->ModelId;

	if (Def->MeshPipeline != nullptr && Def->MeshPipeline->bEnabled)
	{
		Def->MeshPipeline->Apply(Shaped);
	}

	OutModelId = Shaped.ModelId;
	return Shaped.Control;
}

bool UMeshForgeSubsystem::HasReferenceImage(const UMeshDef* Def) const
{
	return Def != nullptr
		&& (!Def->ResolveMainImage().IsNull() || !Def->SourceImagePath.IsEmpty());
}

int32 UMeshForgeSubsystem::UsableExtraViews(const UMeshDef* Def) const
{
	// **One authority, and it is the pipeline.** MaxExtraViews answers zero both when the model
	// cannot read extra views and when multi-view has not been switched on in the model's own
	// settings - which is where that switch lives, beside everything else about the request. It used
	// to live on the definition as well, and two switches meaning the same thing is how a panel
	// starts disagreeing with what was actually sent.
	return Def ? FMath::Min(Def->ExtraViews.Num(), Def->MaxExtraViews()) : 0;
}

UMeshDef* UMeshForgeSubsystem::CreateMeshDef(
	const FString& AssetName, const FMeshDefSpec& Spec, FString& OutError)
{
	const UMeshForgeSettings* Settings = UMeshForgeSettings::Get();

	const FString SafeName = ObjectTools::SanitizeObjectName(
		AssetName.IsEmpty() ? TEXT("MSD_NewMesh") : AssetName);

	const FString PackagePath = Settings->GetDefinitionsPath();

	FAssetToolsModule& AssetTools =
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

	FString UniqueName;
	FString UniquePackage;
	AssetTools.Get().CreateUniqueAssetName(
		PackagePath / SafeName, TEXT(""), UniquePackage, UniqueName);

	UPackage* Package = CreatePackage(*UniquePackage);
	if (Package == nullptr)
	{
		OutError = FString::Printf(TEXT("Could not create a package at '%s'."), *UniquePackage);
		return nullptr;
	}

	UMeshDef* Def = NewObject<UMeshDef>(Package, *UniqueName, RF_Public | RF_Standalone);
	if (Def == nullptr)
	{
		OutError = TEXT("Could not create the mesh definition.");
		return nullptr;
	}

	Def->ApplySpec(Spec);

	FAssetRegistryModule::AssetCreated(Def);
	Package->MarkPackageDirty();

	UE_LOG(LogMeshForge, Log, TEXT("Created mesh definition '%s'."), *UniquePackage);

	return Def;
}

bool UMeshForgeSubsystem::ResolveSourceImage(
	const UMeshDef* Def, TArray<uint8>& OutPng, FString& OutError) const
{
	if (Def == nullptr)
	{
		OutError = TEXT("No definition.");
		return false;
	}

	// Whatever the gallery says is the main image, which is the picture somebody chose by pressing
	// "Use as main" - not necessarily the one in SourceImage, and not necessarily the newest.
	const TSoftObjectPtr<UTexture2D> Chosen = Def->ResolveMainImage();

	if (!Chosen.IsNull())
	{
		if (UTexture2D* Texture = Chosen.LoadSynchronous())
		{
			return FMeshImageIngest::EncodePng(Texture, OutPng, OutError);
		}

		OutError = FString::Printf(TEXT("Could not load the chosen image '%s'."), *Chosen.ToString());
		return false;
	}

	if (!Def->SourceImagePath.IsEmpty())
	{
		if (!FPaths::FileExists(Def->SourceImagePath))
		{
			OutError = FString::Printf(TEXT("No file at '%s'."), *Def->SourceImagePath);
			return false;
		}

		// Handed over as the bytes on disk, whatever format they are. Every provider this was
		// designed against accepts PNG, JPEG and WebP, and re-encoding a JPEG to PNG here would cost
		// a decode and a generation of quality for nothing.
		if (!FFileHelper::LoadFileToArray(OutPng, *Def->SourceImagePath))
		{
			OutError = FString::Printf(TEXT("Could not read '%s'."), *Def->SourceImagePath);
			return false;
		}

		return OutPng.Num() > 0;
	}

	OutError = TEXT("This definition has no source image.");
	return false;
}

int32 UMeshForgeSubsystem::SubmitDef(
	const FString& BatchId, UMeshDef* Def, TSharedPtr<IMeshProvider> Provider, FString& OutError)
{
	const FMeshProviderCaps Caps = Provider->GetCaps();

	FMeshSubmitRequest Base;
	Base.Prompt  = Def->Prompt;
	Base.Control = Def->Control;
	Base.ModelId = Def->ModelId;

	// The pipeline shapes the request it is going to be submitted as - settings, model, and any
	// pictures the texture override needs. ResolveControl runs the same call for pricing, so the
	// estimate and the submission cannot quote different products.
	if (Def->MeshPipeline != nullptr && Def->MeshPipeline->bEnabled)
	{
		Def->MeshPipeline->Apply(Base);
	}

	if (Base.ModelId.IsEmpty())
	{
		Base.ModelId = Provider->GetDefaultModelId();
	}

	FString ImageError;
	const bool bHasImage = ResolveSourceImage(Def, Base.ImagePng, ImageError);

	if (!bHasImage && Caps.bRequiresImage)
	{
		// The interesting case, and the reason IMeshProvider has a concept-image call at all. An
		// image-to-3D model cannot be driven from a prompt - but it may be able to draw one first,
		// which makes a text-only definition work on a provider that has no text path.
		if (Def->Prompt.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("'%s' needs an image and this definition has neither an image nor a prompt (%s)."),
				*Provider->GetDisplayName(), *ImageError);
			return 0;
		}

		// A definition with a concept pipeline is told to run it rather than having it run here.
		//
		// It used to draw inline, and that was the wrong shape once drawing became asynchronous: a
		// submit would either block for two minutes or have to grow its own waiting state. Running
		// the stage explicitly also leaves the picture behind as an asset somebody can look at
		// before spending money reconstructing from it.
		if (Def->ConceptPipeline != nullptr && Def->ConceptPipeline->bEnabled)
		{
			OutError = FString::Printf(
				TEXT("'%s' has no picture chosen yet. Run its Concept stage first - it draws in the "
					 "background - then generate."),
				*Def->GetName());
			return 0;
		}

		if (!Provider->SupportsConceptImages())
		{
			OutError = FString::Printf(
				TEXT("'%s' generates from images only, and this definition has just a prompt. Choose "
					 "an image pipeline on the Concept stage, give it a reference image, or use a "
					 "provider that generates from text."),
				*Provider->GetDisplayName());
			return 0;
		}
		else
		{
			// Synchronously, by waiting on the async call - a submission is already a blocking operation
			// from the caller's point of view and splitting it into two async stages would double the
			// state machine for one extra second of responsiveness.
			bool bDrawn = false;
			bool bDone = false;
			FString DrawError;

			Provider->GenerateConceptImage(Def->Prompt, Def->Control,
				[&Base, &bDrawn, &bDone, &DrawError](bool bSuccess, const TArray<uint8>& Png, const FString& Error)
				{
					bDrawn = bSuccess;
					DrawError = Error;
					if (bSuccess)
					{
						Base.ImagePng = Png;
					}
					bDone = true;
				});

			// Pumping **only HTTP**, never the core ticker.
			//
			// The provider completes on the game thread, so something has to advance it or this waits
			// forever. The obvious thing to turn is the core ticker - and it is the wrong one: this
			// subsystem's own poll runs on that ticker, so turning it re-enters Poll while we are inside
			// GenerateMeshes holding references into the batch map. Poll retires finished batches, which
			// means it can erase the very entry the outer frame is still using.
			//
			// The HTTP manager is what actually delivers the reply, and turning it advances nothing else.
			FHttpModule& Http = FHttpModule::Get();

			const double Deadline = FPlatformTime::Seconds() + 300.0;
			while (!bDone && FPlatformTime::Seconds() < Deadline)
			{
				FPlatformProcess::Sleep(0.05f);
				Http.GetHttpManager().Tick(0.05f);
			}

			if (!bDrawn)
			{
				OutError = FString::Printf(TEXT("Could not draw a reference image for '%s': %s"),
					*Def->GetName(), *DrawError);
				return 0;
			}

			UE_LOG(LogMeshForge, Log,
				TEXT("Drew a reference image for '%s' from its prompt."), *Def->GetName());
		}
	}

	if (!bHasImage && !Caps.bSupportsTextPrompt && Base.ImagePng.Num() == 0)
	{
		OutError = FString::Printf(TEXT("'%s' cannot generate from a prompt alone."),
			*Provider->GetDisplayName());
		return 0;
	}

	// Extra views, as many as this definition will actually send. **Asked of one function rather
	// than worked out here**, because the answer depends on three things - the switch, the model the
	// pipeline names, and how many views exist - and the panel has to give the same answer this does
	// or it is lying about what was generated.
	const int32 Sending = UsableExtraViews(Def);

	if (Def->ExtraViews.Num() > 0)
	{
		if (Sending < Def->ExtraViews.Num())
		{
			// Said out loud every time, because pictures being silently ignored looks exactly like
			// pictures being used badly - and on a metered service they were paid for.
			UE_LOG(LogMeshForge, Log,
				TEXT("'%s': sending %d of %d extra view(s)%s."),
				*Def->GetName(), Sending, Def->ExtraViews.Num(),
				TEXT(" - the model's settings read no more"));
		}

		if (Sending > 0)
		{
			for (const TSoftObjectPtr<UTexture2D>& View : Def->ExtraViews)
			{
				if (Base.AdditionalImagesPng.Num() >= Sending)
				{
					break;
				}

				UTexture2D* Texture = View.LoadSynchronous();

				TArray<uint8> Png;
				FString ViewError;

				if (Texture && FMeshImageIngest::EncodePng(Texture, Png, ViewError))
				{
					Base.AdditionalImagesPng.Add(MoveTemp(Png));
				}
				else
				{
					// Not fatal. A view that cannot be read is one angle missing, and failing the
					// whole submission over it throws away a good main image for a bad extra.
					UE_LOG(LogMeshForge, Warning, TEXT("'%s': skipping an extra view - %s"),
						*Def->GetName(), *ViewError);
				}
			}
		}
	}

	// A provider that ignores text entirely should not be handed a prompt that will vanish silently.
	if (Base.ImagePng.Num() > 0 && !Caps.bSupportsTextWithImage && !Def->Prompt.IsEmpty())
	{
		UE_LOG(LogMeshForge, Log,
			TEXT("'%s' generates from the image only; the prompt on '%s' is not used."),
			*Provider->GetDisplayName(), *Def->GetName());
		Base.Prompt.Empty();
	}

	const int32 Wanted = Caps.bSupportsVariants ? FMath::Max(1, Def->Variants) : 1;

	if (Wanted < Def->Variants)
	{
		UE_LOG(LogMeshForge, Log,
			TEXT("'%s' generates one take at a time; %d were asked for and one was submitted."),
			*Provider->GetDisplayName(), Def->Variants);
	}

	FBatch& Batch = Batches.FindOrAdd(BatchId);
	int32 Started = 0;

	for (int32 Variant = 0; Variant < Wanted; ++Variant)
	{
		FMeshSubmitRequest Request = Base;
		Request.VariantIndex = Variant;

		// Variants walk the seed rather than repeating it, so one seed gives several different but
		// reproducible takes. Repeating it would generate the same mesh N times and bill for each.
		if (Request.Control.bUseSeed)
		{
			Request.Control.Seed = Base.Control.Seed + Variant;
		}

		FMeshCandidate Candidate;
		Candidate.VariantIndex = Variant;
		Candidate.Status = EMeshJobStatus::Pending;
		Candidate.ProviderId = Provider->GetProviderId();
		Candidate.ModelId = Request.ModelId;
		Candidate.Quality = Request.Control.Quality;
		Candidate.Seed = Request.Control.Seed;
		Candidate.GeneratedAt = FDateTime::UtcNow();

		// Recorded before the provider answers, so a submission that fails still leaves a trace of
		// what was attempted. A candidate list that only holds successes cannot explain a bill.
		const int32 CandidateIndex = Def->Candidates.Add(Candidate);

		TWeakObjectPtr<UMeshDef> WeakDef(Def);
		const FString CapturedBatch = BatchId;

		// Marked in flight *before* the call, so the poll cannot retire this batch while the
		// provider is still thinking. See FBatch::Submitting.
		++Batch.Submitting;

		Provider->SubmitJob(Request, [this, WeakDef, CapturedBatch, CandidateIndex]
			(const FMeshSubmitResult& Result)
		{
			// First, and before any early return. A submission that comes back to a definition
			// somebody deleted still has to stop being in flight, or the batch stays live forever
			// and the poll never retires it.
			if (FBatch* Batch = Batches.Find(CapturedBatch))
			{
				Batch->Submitting = FMath::Max(0, Batch->Submitting - 1);
			}

			UMeshDef* Target = WeakDef.Get();
			if (Target == nullptr || !Target->Candidates.IsValidIndex(CandidateIndex))
			{
				return;
			}

			FMeshCandidate& Slot = Target->Candidates[CandidateIndex];

			if (!Result.bSuccess)
			{
				Slot.Status = EMeshJobStatus::Failed;
				Slot.Error = Result.Error;
				Target->SetStatus(EMeshDefStatus::Failed, Result.Error);
				return;
			}

			Slot.JobId = Result.JobId;
			Slot.Status = EMeshJobStatus::Running;

			// The provider's seed wins where it picked one, so the record is what actually happened
			// rather than what was asked for.
			if (Result.Seed != 0)
			{
				Slot.Seed = Result.Seed;
			}

			if (FBatch* Live = Batches.Find(CapturedBatch))
			{
				FTrackedJob Job;
				Job.Def = WeakDef;
				Job.ProviderId = Slot.ProviderId;
				Job.JobId = Result.JobId;
				Job.VariantIndex = Slot.VariantIndex;
				Job.StartedAt = FPlatformTime::Seconds();
				Live->Jobs.Add(Job);
			}

			Target->SetStatus(EMeshDefStatus::Generating);
			Target->MarkPackageDirty();
		});

		++Started;
	}

	Def->ActiveBatchId = BatchId;
	Def->SetStatus(EMeshDefStatus::Submitting);

	return Started;
}

FMeshBatchSubmission UMeshForgeSubsystem::GenerateMeshes(const TArray<UMeshDef*>& Defs)
{
	FMeshBatchSubmission Submission;

	if (Defs.Num() == 0)
	{
		Submission.LastError = TEXT("Nothing to generate.");
		return Submission;
	}

	const FString BatchId = FGuid::NewGuid().ToString(EGuidFormats::Short);

	FBatch& Batch = Batches.Add(BatchId);
	Batch.StartedAt = FPlatformTime::Seconds();

	for (UMeshDef* Def : Defs)
	{
		if (Def == nullptr)
		{
			continue;
		}

		if (!Def->HasInput())
		{
			++Submission.Failed;
			Submission.LastError = FString::Printf(
				TEXT("'%s' has neither a prompt nor an image."), *Def->GetName());
			Def->SetStatus(EMeshDefStatus::Failed, Submission.LastError);
			continue;
		}

		TSharedPtr<IMeshProvider> Provider = ResolveProvider(Def);

		if (!Provider.IsValid())
		{
			++Submission.Failed;
			Submission.LastError = FString::Printf(
				TEXT("No mesh provider for '%s'. Enable a provider plugin and set one in Project "
					 "Settings, or on the definition."),
				*Def->GetName());
			Def->SetStatus(EMeshDefStatus::Failed, Submission.LastError);
			continue;
		}

		FString Reason;
		if (!Provider->IsAvailable(Reason))
		{
			++Submission.Failed;
			Submission.LastError = FString::Printf(TEXT("'%s' is not ready: %s"),
				*Provider->GetDisplayName(), *Reason);
			Def->SetStatus(EMeshDefStatus::Failed, Submission.LastError);
			continue;
		}

		FString SubmitError;
		const int32 Started = SubmitDef(BatchId, Def, Provider, SubmitError);

		if (Started == 0)
		{
			++Submission.Failed;
			Submission.LastError = SubmitError;
			Def->SetStatus(EMeshDefStatus::Failed, SubmitError);
			continue;
		}

		Submission.Submitted += Started;

		// One job per definition rather than per variant. Four takes of one prop is one thing
		// somebody is waiting for, and four identical lines would push everything else off the strip.
		// A no-op when StartMeshGeneration already registered one, which is the panel's route in.
		BeginMeshJob(Def);
	}

	if (Submission.Submitted == 0)
	{
		// The batch id stays empty on purpose. A caller that checks it first cannot then read the
		// counts and conclude something is in flight when nothing is.
		Batches.Remove(BatchId);
		return Submission;
	}

	Submission.BatchId = BatchId;

	UE_LOG(LogMeshForge, Log, TEXT("Batch %s: %d submitted, %d failed."),
		*BatchId, Submission.Submitted, Submission.Failed);

	return Submission;
}

FMeshBatchSubmission UMeshForgeSubsystem::GenerateMesh(UMeshDef* Def)
{
	return GenerateMeshes({ Def });
}

bool UMeshForgeSubsystem::IsBatchRunning(const FString& BatchId) const
{
	const FBatch* Batch = Batches.Find(BatchId);
	if (Batch == nullptr)
	{
		return false;
	}

	// Importing counts too: a batch whose provider work is done but whose meshes are still being
	// written has not finished, whatever the jobs say.
	return Batch->Submitting > 0
		|| Batch->Importing > 0
		|| Batch->Jobs.ContainsByPredicate([](const FTrackedJob& Job) { return !Job.bFinished; });
}

void UMeshForgeSubsystem::CancelBatch(const FString& BatchId)
{
	FBatch* Batch = Batches.Find(BatchId);
	if (Batch == nullptr)
	{
		return;
	}

	Batch->bCancelled = true;

	for (FTrackedJob& Job : Batch->Jobs)
	{
		if (Job.bFinished)
		{
			continue;
		}

		Job.bFinished = true;

		if (UMeshDef* Def = Job.Def.Get())
		{
			if (FMeshCandidate* Candidate = Def->FindCandidateByJobMutable(Job.JobId))
			{
				Candidate->Status = EMeshJobStatus::Cancelled;
			}

			Def->ActiveBatchId.Empty();
			Def->SetStatus(EMeshDefStatus::Draft);
		}
	}

	UE_LOG(LogMeshForge, Log, TEXT("Batch %s cancelled."), *BatchId);
	Batches.Remove(BatchId);
}

void UMeshForgeSubsystem::ReportProgress(const FString& BatchId)
{
	const FBatch* Batch = Batches.Find(BatchId);
	if (Batch == nullptr)
	{
		return;
	}

	int32 Finished = 0;
	for (const FTrackedJob& Job : Batch->Jobs)
	{
		Finished += Job.bFinished ? 1 : 0;
	}

	OnBatchProgress.Broadcast(BatchId, Finished, Batch->Jobs.Num());
}

void UMeshForgeSubsystem::FinishJob(
	const FString& BatchId, FTrackedJob& Job, bool bSuccess, const FString& Error)
{
	Job.bFinished = true;

	UMeshDef* Def = Job.Def.Get();
	if (Def == nullptr)
	{
		return;
	}

	if (!bSuccess)
	{
		Def->SetStatus(EMeshDefStatus::Failed, Error);
		OnDefFinished.Broadcast(Def, false);
		return;
	}

	Def->SetStatus(EMeshDefStatus::Generated);

	// The take that just finished, which is the one this definition now means.
	//
	// **This is the fix for a definition regenerating and importing its previous mesh.** The
	// selection used to be written only when nothing was selected yet - right for a fresh
	// definition, wrong for every regeneration after it - and the import then asked for "whatever
	// this definition would use", got the *old* id, and re-imported the old artifact. A Meshy
	// generation that succeeded and was billed came back as the local mesh from the run before,
	// with a triangle count identical to it, which is how it was found.
	const FMeshCandidate* Finished = Def->FindCandidateByJobMutable(Job.JobId);
	const FString FinishedId = Finished ? Finished->MeshId : FString();

	if (!FinishedId.IsEmpty())
	{
		Def->SelectedMeshId = FinishedId;
	}

	const UMeshForgeSettings* Settings = UMeshForgeSettings::Get();

	if (!Settings->bAutoImportFirstSuccess)
	{
		OnDefFinished.Broadcast(Def, true);
		return;
	}

	// The first success *in this batch* is imported and the rest are left as candidates. Importing
	// every variant would create four static meshes for a definition that describes one prop; keying
	// this on whether the definition has any imported mesh at all would instead make every
	// regeneration a no-op, which is worse - regenerating is how somebody changes a mesh.
	FBatch* Batch = Batches.Find(BatchId);

	if (Batch != nullptr && Batch->Imported.Contains(Def))
	{
		OnDefFinished.Broadcast(Def, true);
		return;
	}

	if (Batch != nullptr)
	{
		Batch->Imported.Add(Def);
	}

	if (Batch != nullptr)
	{
		Batch->Importing++;
	}

	TWeakObjectPtr<UMeshDef> WeakDef(Def);
	const FString ImportBatchId = BatchId;

	// Named explicitly. Passing an empty id means "re-resolve", and re-resolving is exactly what let
	// a stale selection win over the take that had just been generated and paid for.
	ImportCandidate(Def, FinishedId, [this, WeakDef, ImportBatchId](const FMeshImportOutcome& Outcome)
	{
		// Decrement first, and on every path: a counter that leaks on failure keeps the batch alive
		// forever, which is a worse bug than the one it was added to fix.
		if (FBatch* Owner = Batches.Find(ImportBatchId))
		{
			Owner->Importing = FMath::Max(0, Owner->Importing - 1);
		}

		if (UMeshDef* Target = WeakDef.Get())
		{
			OnDefFinished.Broadcast(Target, Outcome.bSuccess);
		}
	});
}

bool UMeshForgeSubsystem::Poll(float DeltaTime)
{
	const UMeshForgeSettings* Settings = UMeshForgeSettings::Get();

	const double Now = FPlatformTime::Seconds();
	if (Now - LastPollAt < static_cast<double>(FMath::Max(1, Settings->PollIntervalSeconds)))
	{
		return true;
	}
	LastPollAt = Now;

	FMeshForgeModule* Module = FMeshForgeModule::GetPtrIfLoaded();
	if (Module == nullptr)
	{
		return true;
	}

	TArray<FString> BatchIds;
	Batches.GetKeys(BatchIds);

	for (const FString& BatchId : BatchIds)
	{
		FBatch* Batch = Batches.Find(BatchId);
		if (Batch == nullptr || Batch->bCancelled)
		{
			continue;
		}

		for (int32 Index = 0; Index < Batch->Jobs.Num(); ++Index)
		{
			FTrackedJob& Job = Batch->Jobs[Index];

			if (Job.bFinished)
			{
				continue;
			}

			if (Settings->JobTimeoutSeconds > 0
				&& Now - Job.StartedAt > static_cast<double>(Settings->JobTimeoutSeconds))
			{
				FinishJob(BatchId, Job, false, FString::Printf(
					TEXT("Gave up after %d seconds. The job may still be running on the provider."),
					Settings->JobTimeoutSeconds));
				continue;
			}

			TSharedPtr<IMeshProvider> Provider = Module->FindProvider(Job.ProviderId);
			if (!Provider.IsValid())
			{
				FinishJob(BatchId, Job, false, TEXT("The provider was unloaded while this job was running."));
				continue;
			}

			const FString CapturedBatch = BatchId;
			const int32 CapturedIndex = Index;

			Provider->PollJob(Job.JobId, [this, CapturedBatch, CapturedIndex](const FMeshJobResult& Result)
			{
				FBatch* Live = Batches.Find(CapturedBatch);
				if (Live == nullptr || !Live->Jobs.IsValidIndex(CapturedIndex))
				{
					return;
				}

				FTrackedJob& LiveJob = Live->Jobs[CapturedIndex];
				if (LiveJob.bFinished)
				{
					return;
				}

				UMeshDef* Def = LiveJob.Def.Get();
				if (Def == nullptr)
				{
					LiveJob.bFinished = true;
					return;
				}

				FMeshCandidate* Candidate = Def->FindCandidateByJobMutable(LiveJob.JobId);

				// **How long it took, measured here because no provider reports it.**
				//
				// GenerationSeconds is a field every vendor could fill and none does, so every take
				// in the library read "0s" - and how long a setting takes is one of the two numbers
				// worth comparing between takes, the other being what it cost.
				//
				// Measured from the moment the provider accepted the submission to the poll that
				// saw it finish, so it is rounded up by at most one poll interval and does not
				// include our own upload. The provider's own figure still wins where one arrives.
				const float Elapsed = static_cast<float>(FPlatformTime::Seconds() - LiveJob.StartedAt);

				const float Measured = (Result.GenerationSeconds > 0.f)
					? Result.GenerationSeconds
					: FMath::Max(0.f, Elapsed);

				switch (Result.Status)
				{
				case EMeshJobStatus::Succeeded:
					if (Candidate != nullptr)
					{
						Candidate->Status = EMeshJobStatus::Succeeded;
						Candidate->MeshId = Result.MeshId;
						Candidate->TriangleCount = Result.TriangleCount;
						Candidate->GenerationSeconds = Measured;
						Candidate->ConsumedCredits = Result.ConsumedCredits;

						// The selection is decided by FinishJob, which is also what decides which take
						// gets imported - so the two cannot disagree. It used to be set here, and
						// only when nothing was selected yet, which is the bug below.
					}
					FinishJob(CapturedBatch, LiveJob, true, FString());
					break;

				case EMeshJobStatus::Failed:
					if (Candidate != nullptr)
					{
						Candidate->Status = EMeshJobStatus::Failed;
						Candidate->Error = Result.Error;

						// A failure that took four minutes is a different fact from one that came
						// back at once, and the second is usually a request the vendor refused.
						Candidate->GenerationSeconds = Measured;
					}
					FinishJob(CapturedBatch, LiveJob, false, Result.Error);
					break;

				default:
					// Still running. Nothing to record; the status is already Running.
					break;
				}

				ReportProgress(CapturedBatch);
			});
		}

		// Retire a batch once nothing in it is live, so the map does not grow for a session.
		if (!Batch->IsLive())
		{
			for (const FTrackedJob& Job : Batch->Jobs)
			{
				if (UMeshDef* Def = Job.Def.Get())
				{
					Def->ActiveBatchId.Empty();
				}
			}

			UE_LOG(LogMeshForge, Log, TEXT("Batch %s finished."), *BatchId);
			Batches.Remove(BatchId);
		}
	}

	return true;
}

FForgeTakeRecord UMeshForgeSubsystem::BuildTakeRecord(
	const UMeshDef* Def, const FMeshCandidate& Candidate) const
{
	FForgeTakeRecord Record;

	Record.TakeId  = Candidate.MeshId;
	Record.Plugin  = TEXT("MeshForge");
	Record.Kind    = TEXT("mesh");

	Record.DefinitionPath = Def->GetPathName();
	Record.DefinitionName = Def->GetName();

	Record.ProviderId = Candidate.ProviderId.ToString();
	Record.ModelId    = Candidate.ModelId;

	FString Model;
	const FMeshControl Control = ResolveControl(Def, Model);

	Record.Prompt = Def->Prompt;

	// What was actually sent, not what the definition happens to hold now. The two differ the moment
	// a pipeline is applied over the definition's advanced settings, and the record has to be the
	// request rather than the asset's current state.
	Record.Settings.Add(TEXT("quality"),
		StaticEnum<EMeshQuality>()->GetNameStringByValue(static_cast<int64>(Control.Quality)));
	Record.Settings.Add(TEXT("textureSize"), FString::FromInt(Control.TextureSize));
	Record.Settings.Add(TEXT("targetTriangles"), FString::FromInt(Control.TargetTriangles));
	Record.Settings.Add(TEXT("remesh"), Control.bRemesh ? TEXT("true") : TEXT("false"));
	Record.Settings.Add(TEXT("removeBackground"),
		Control.bRemoveBackground ? TEXT("true") : TEXT("false"));

	if (Control.bUseSeed)
	{
		Record.Settings.Add(TEXT("seed"), FString::FromInt(Control.Seed));
	}

	for (const TPair<FString, FString>& Extra : Control.Extra)
	{
		Record.Settings.Add(Extra.Key, Extra.Value);
	}

	// The pictures the provider was shown, by content path and hash. Hashed rather than only named,
	// because a texture that is later edited or moved would otherwise silently change what the
	// record claims went in.
	auto AddImage = [&Record](const TSoftObjectPtr<UTexture2D>& Image, const TCHAR* Role)
	{
		if (Image.IsNull())
		{
			return;
		}

		FForgeTakeInput Input;
		Input.Role      = Role;
		Input.AssetPath = Image.ToString();

		if (UTexture2D* Texture = Image.LoadSynchronous())
		{
			TArray<uint8> Png;
			FString Ignored;

			if (FMeshImageIngest::EncodePng(Texture, Png, Ignored))
			{
				Input.Hash = FForgeLibrary::HashBytes(Png);
			}
		}

		Record.Inputs.Add(Input);
	};

	AddImage(Def->ResolveMainImage(), TEXT("main"));

	for (int32 Index = 0; Index < Def->ExtraViews.Num(); ++Index)
	{
		AddImage(Def->ExtraViews[Index], *FString::Printf(TEXT("view%d"), Index + 1));
	}

	if (const TSharedPtr<IMeshProvider> Provider = ResolveProvider(Def))
	{
		const FMeshProviderCaps Caps = Provider->GetCaps();
		Record.bLocal = !Caps.bIsMetered;

		// The estimate, flagged as one. Nothing here has seen a receipt, and a ledger that cannot
		// tell a quote from an invoice will be believed anyway.
		const FString Cost = Provider->DescribeCost(
			Control, HasReferenceImage(Def), UsableExtraViews(Def));

		if (!Cost.IsEmpty())
		{
			Record.CostCurrency  = Cost;
			Record.bCostEstimated = true;
		}

		// The vendor's own figure wins where it gave one. This is the difference between a ledger
		// that can be reconciled against an invoice and one that can only be argued with.
		if (Candidate.ConsumedCredits >= 0)
		{
			Record.CostCurrency   = TEXT("credits");
			Record.CostAmount     = Candidate.ConsumedCredits;
			Record.bCostEstimated = false;
		}

		// And in money, where the vendor publishes a rate. Credits are not comparable between
		// vendors - thirty of one and thirty of another are different amounts - so a ledger that
		// only counts credits cannot be added up, which is the one thing a ledger is for.
		//
		// Zero where the vendor publishes nothing, and the panel says so rather than guessing.
		const double UsdPerCredit = Provider->GetUsdPerCredit();

		if (UsdPerCredit > 0.0 && Record.CostAmount > 0.0)
		{
			Record.CostUsd = Record.CostAmount * UsdPerCredit;
		}
	}

	Record.StartedUtc  = Candidate.GeneratedAt;
	Record.FinishedUtc = FDateTime::UtcNow();
	Record.Seconds     = Candidate.GenerationSeconds;

	Record.Status = Candidate.IsUsable() ? TEXT("succeeded") : TEXT("failed");
	Record.Error  = Candidate.Error;

	Record.ToolVersion = TEXT("MeshForge");

	return Record;
}

void UMeshForgeSubsystem::ImportCandidate(
	UMeshDef* Def, const FString& MeshId, TFunction<void(const FMeshImportOutcome&)> OnDone)
{
	FMeshImportOutcome Failure;

	if (Def == nullptr)
	{
		Failure.Error = TEXT("No definition.");
		OnDone(Failure);
		return;
	}

	// An empty id means "whatever this definition would use": the chosen take, or the only one that
	// worked. Making the caller work that out would put the same three lines at every call site.
	FString TargetId = MeshId;

	if (TargetId.IsEmpty())
	{
		TargetId = Def->SelectedMeshId;
	}

	if (TargetId.IsEmpty())
	{
		for (const FMeshCandidate& Candidate : Def->Candidates)
		{
			if (Candidate.IsUsable())
			{
				TargetId = Candidate.MeshId;
				break;
			}
		}
	}

	const FMeshCandidate* Candidate = Def->FindCandidate(TargetId);

	if (Candidate == nullptr || !Candidate->IsUsable())
	{
		Failure.Error = FString::Printf(
			TEXT("'%s' has no finished take to import."), *Def->GetName());
		Def->SetStatus(EMeshDefStatus::Failed, Failure.Error);
		OnDone(Failure);
		return;
	}

	FMeshForgeModule* Module = FMeshForgeModule::GetPtr();
	TSharedPtr<IMeshProvider> Provider = Module ? Module->FindProvider(Candidate->ProviderId) : nullptr;

	if (!Provider.IsValid())
	{
		Failure.Error = FString::Printf(
			TEXT("The provider that made this take ('%s') is not loaded."),
			*Candidate->ProviderId.ToString());
		OnDone(Failure);
		return;
	}

	const UMeshForgeSettings* Settings = UMeshForgeSettings::Get();

	const FString Staging = Settings->GetStagingDirectory();
	IFileManager::Get().MakeDirectory(*Staging, true);

	// Staged per *take*, not per definition. It used to be one `MSD_PanelTest.glb` overwritten on
	// every run, so a definition's own candidate list pointed at artifacts that no longer existed -
	// which is half of why a regenerated definition could come back with the previous mesh.
	// Asked per job rather than per provider: quad topology comes back as FBX on at least one
	// vendor, and staging it under a .glb name would hand Interchange a file whose contents and
	// extension disagree.
	const FString ArtifactPath = Staging / FString::Printf(TEXT("%s.%s"),
		*ObjectTools::SanitizeObjectName(TargetId),
		*Provider->GetArtifactExtensionForJob(TargetId));

	TWeakObjectPtr<UMeshDef> WeakDef(Def);
	const FString CapturedId = TargetId;

	Provider->DownloadMesh(TargetId, ArtifactPath,
		[this, WeakDef, CapturedId, ArtifactPath, OnDone](bool bSuccess, const FString& Error)
	{
		FMeshImportOutcome Outcome;

		UMeshDef* Target = WeakDef.Get();
		if (Target == nullptr)
		{
			Outcome.Error = TEXT("The definition was destroyed while its mesh was downloading.");
			OnDone(Outcome);
			return;
		}

		if (!bSuccess)
		{
			Outcome.Error = FString::Printf(TEXT("Download failed: %s"), *Error);
			Target->SetStatus(EMeshDefStatus::Failed, Outcome.Error);
			OnDone(Outcome);
			return;
		}

		// **Filed before anything is imported.** An import can fail, an editor can be closed and a
		// definition can be deleted; none of that may lose something that was paid for. This is the
		// whole point of the vault - see PROVENANCE_CONTRACT.md.
		FString VaultArtifact = ArtifactPath;

		if (FMeshCandidate* Candidate = Target->FindCandidateMutable(CapturedId))
		{
			Candidate->LocalArtifactPath = ArtifactPath;

			FForgeTakeRecord Record = BuildTakeRecord(Target, *Candidate);

			FString VaultError;

			if (FForgeLibrary::Deposit(Record, ArtifactPath, VaultError))
			{
				// Imported from the vault copy rather than from staging, so what lands in the
				// project is demonstrably the bytes the record hashed.
				VaultArtifact = Record.ArtifactPath();
				Candidate->LocalArtifactPath = VaultArtifact;
			}
			else
			{
				// Not fatal. A take that could not be filed is still a take that can be imported,
				// and refusing here would throw away the expensive thing to protect the record of it.
				UE_LOG(LogMeshForge, Warning,
					TEXT("'%s': could not file take %s in the library - %s"),
					*Target->GetName(), *CapturedId, *VaultError);
			}
		}

		Target->SetStatus(EMeshDefStatus::Downloaded);

		// The import's own clock. Not the generation's - carrying its minutes onto this stage would
		// make re-finishing, which is free and takes a second, look expensive.
		const double ImportStarted = FPlatformTime::Seconds();

		const UMeshForgeSettings* Settings = UMeshForgeSettings::Get();

		FMeshImportRequest Request;
		Request.AbsoluteArtifactPath = VaultArtifact;

		// **This take's own folder, and the collision it prevents is not hypothetical.** Artifacts
		// are filed under a fixed name, so Interchange - which names its destination after the source
		// file - previously put every take of every definition into one folder. Two takes then shared
		// a material, and importing the second created its textures without repointing that material:
		// the project kept rendering the first take's textures while the new ones sat beside them.
		Request.MeshPackagePath = Settings->GetMeshTakeFolder(Target->GetName(), CapturedId);
		Request.Finish = Target->Finish;
		Request.NaniteTriangleThreshold = Settings->NaniteTriangleThreshold;
		Request.bSaveAssets = Settings->bSaveGeneratedAssets;

		// Named after the definition rather than after the take, so regenerating a definition
		// replaces its mesh instead of littering the folder with SM_Crate_1, _2, _3.
		Request.AssetName = FString::Printf(TEXT("SM_%s"),
			*Target->GetName().Replace(TEXT("MSD_"), TEXT(""), ESearchCase::CaseSensitive));

		Outcome = FMeshImporter::Import(Request);

		Target->LastImport = Outcome;

		// The Import stage is recorded *here*, where an import actually happened - not wherever a
		// definition reaches a terminal state. Doing it there marked Import ready on any success
		// where the definition merely had an old mesh from a previous run, claiming a finish that
		// matched a candidate it was never made from.
		FMeshStageState& Stage = Target->Stages.FindOrAdd(EMeshStage::Import);
		Stage.LastRunUtc     = FDateTime::UtcNow();
		Stage.LastRunSeconds = static_cast<float>(FPlatformTime::Seconds() - ImportStarted);

		if (Outcome.bSuccess)
		{
			Target->ImportedMesh = Outcome.Mesh;
			Target->SetStatus(EMeshDefStatus::Imported);

			Stage.Status     = EMeshStageStatus::Ready;
			Stage.InputsHash = Target->ComputeStageHash(EMeshStage::Import);
			Stage.Error.Reset();

			for (const FString& Warning : Outcome.Warnings)
			{
				UE_LOG(LogMeshForge, Warning, TEXT("'%s': %s"), *Target->GetName(), *Warning);
			}

			// Appended to the take, so the record says what became of it as well as what made it.
			if (const FMeshCandidate* Filed = Target->FindCandidate(CapturedId))
			{
				const FString Directory = FForgeLibrary::TakeDirectory(
					TEXT("MeshForge"), Target->GetName(), Filed->MeshId);

				FForgeProcessingEvent Event;
				Event.StepId  = TEXT("unreal.import");
				Event.Tool    = TEXT("MeshForge");
				Event.AtUtc   = FDateTime::UtcNow();
				Event.Summary = FString::Printf(
					TEXT("%d triangles, %d LODs, %d collision primitive(s), Nanite %s"),
					Outcome.TriangleCount, Outcome.LodCount, Outcome.CollisionPrimitives,
					Outcome.bNaniteEnabled ? TEXT("on") : TEXT("off"));

				FString AppendError;
				FForgeLibrary::AppendProcessing(Directory, Event, AppendError);
			}
		}
		else
		{
			Target->SetStatus(EMeshDefStatus::Failed, Outcome.Error);

			Stage.Status = EMeshStageStatus::Failed;
			Stage.Error  = Outcome.Error;
		}

		Target->MarkPackageDirty();
		OnDone(Outcome);
	});
}


FMeshImportOutcome UMeshForgeSubsystem::ImportTake(UMeshDef* Def, const FString& TakeId)
{
	FMeshImportOutcome Outcome;

	if (Def == nullptr || TakeId.IsEmpty())
	{
		Outcome.Error = TEXT("No take to import.");
		return Outcome;
	}

	// --- the library, not the candidate list ------------------------------------------------------
	//
	// **This used to go through ImportCandidate and that was wrong.** A candidate is what the mesh
	// stage recorded on the definition during one generation; the library holds every take ever
	// filed, including post-processed ones which are not candidates at all, and takes belonging to
	// a definition that has since been duplicated, reloaded or regenerated. So the Takes tab could
	// list four takes, all present on disk, and refuse every one of them with "no finished take to
	// import" - which is exactly what somebody sees before they conclude the library is decorative.
	//
	// The library entry is the authority here. It knows the artifact, the kind, and what made it.
	const FString Directory = FForgeLibrary::TakeDirectory(TEXT("MeshForge"), Def->GetName(), TakeId);

	FForgeTakeRecord Record;

	if (!FForgeLibrary::ReadRecord(Directory, Record))
	{
		Outcome.Error = FString::Printf(
			TEXT("There is no record of take %s for '%s' in the Forge library."),
			*TakeId, *Def->GetName());
		return Outcome;
	}

	const FString Artifact = Record.ArtifactPath();

	if (Artifact.IsEmpty() || !FPaths::FileExists(Artifact))
	{
		// **Route A has failed and the caller needs to know which route is left.** The record
		// survives a cleaned artifact deliberately, so this is a recoverable state rather than a
		// lost one wherever the provider still holds the task - and a sentence that says so is the
		// difference between somebody re-fetching it and somebody regenerating it for money.
		Outcome.Error = FString::Printf(
			TEXT("Take %s is recorded but its file is not on this disk. It was made on '%s'%s - "
				 "fetch it again from there, or regenerate. Nothing is lost while that provider "
				 "still knows the task."),
			*TakeId,
			Record.ProviderId.IsEmpty() ? TEXT("an unknown provider") : *Record.ProviderId,
			Record.bLocal ? TEXT(" on this machine") : TEXT(""));
		return Outcome;
	}

	const UMeshForgeSettings* Settings = UMeshForgeSettings::Get();

	// Post takes and generated takes are the same thing to import and different things to file, so
	// the folder follows the kind the record already carries rather than being guessed here.
	const bool bIsPost = Record.Kind == TEXT("mesh-post");

	const FString Folder = bIsPost
		? Settings->GetPostTakeFolder(Def->GetName(), TakeId)
		: Settings->GetMeshTakeFolder(Def->GetName(), TakeId);

	const FString AssetName = FString::Printf(TEXT("SM_%s"),
		*Def->GetName().Replace(TEXT("MSD_"), TEXT(""), ESearchCase::CaseSensitive));

	// --- already here? then choosing it is free and instant --------------------------------------
	//
	// Each take owns its folder, so a take that has been imported before is still sitting there
	// intact. Re-importing would spend seconds rebuilding assets that already exist and, worse,
	// churn their guids - so choosing between takes somebody has already looked at costs nothing.
	// A take whose folder has been cleaned falls through and is rebuilt from the library below.
	const FString ExistingPath = FString::Printf(TEXT("%s/StaticMeshes/%s.%s"),
		*Folder, *AssetName, *AssetName);

	if (UStaticMesh* Existing = LoadObject<UStaticMesh>(nullptr, *ExistingPath))
	{
		Def->ImportedMesh = Existing;

		if (!bIsPost && Def->FindCandidate(TakeId) != nullptr)
		{
			Def->SelectedMeshId = TakeId;
		}

		Def->SetStatus(EMeshDefStatus::Imported);
		Def->MarkPackageDirty();

		Outcome = Def->LastImport;
		Outcome.bSuccess = true;
		Outcome.Mesh     = Existing;

		UE_LOG(LogMeshForge, Log,
			TEXT("'%s': take %s is already imported - selected it without rebuilding."),
			*Def->GetName(), *TakeId);

		return Outcome;
	}

	FMeshImportRequest Request;
	Request.AbsoluteArtifactPath    = Artifact;
	Request.MeshPackagePath         = Folder;
	Request.Finish                  = Def->Finish;
	Request.NaniteTriangleThreshold = Settings->NaniteTriangleThreshold;
	Request.bSaveAssets             = Settings->bSaveGeneratedAssets;
	Request.AssetName               = AssetName;

	const double Started = FPlatformTime::Seconds();

	Outcome = FMeshImporter::Import(Request);

	Def->LastImport = Outcome;

	FMeshStageState& Stage = Def->Stages.FindOrAdd(EMeshStage::Import);
	Stage.LastRunUtc     = FDateTime::UtcNow();
	Stage.LastRunSeconds = static_cast<float>(FPlatformTime::Seconds() - Started);

	if (Outcome.bSuccess)
	{
		Def->ImportedMesh = Outcome.Mesh;

		// Only where this take is one the mesh stage produced. Choosing a post-processed take does
		// not rewrite which *generation* the definition selected - those are different questions,
		// and conflating them would silently change what a regenerate would compare against.
		if (!bIsPost && Def->FindCandidate(TakeId) != nullptr)
		{
			Def->SelectedMeshId = TakeId;
		}

		Def->SetStatus(EMeshDefStatus::Imported);

		Stage.Status     = EMeshStageStatus::Ready;
		Stage.InputsHash = Def->ComputeStageHash(EMeshStage::Import);
		Stage.Error.Reset();

		for (const FString& Warning : Outcome.Warnings)
		{
			UE_LOG(LogMeshForge, Warning, TEXT("'%s': %s"), *Def->GetName(), *Warning);
		}

		UE_LOG(LogMeshForge, Log, TEXT("'%s': imported take %s from the library (%d triangles)."),
			*Def->GetName(), *TakeId, Outcome.TriangleCount);
	}
	else
	{
		Def->SetStatus(EMeshDefStatus::Failed, Outcome.Error);

		Stage.Status = EMeshStageStatus::Failed;
		Stage.Error  = Outcome.Error;
	}

	Def->MarkPackageDirty();

	return Outcome;
}

FMeshImportOutcome UMeshForgeSubsystem::RefinishMesh(UMeshDef* Def)
{
	FMeshImportOutcome Outcome;

	if (Def == nullptr)
	{
		Outcome.Error = TEXT("No definition.");
		return Outcome;
	}

	UStaticMesh* Mesh = Def->ImportedMesh.LoadSynchronous();

	if (Mesh == nullptr)
	{
		Outcome.Error = FString::Printf(
			TEXT("'%s' has no imported mesh to finish."), *Def->GetName());
		return Outcome;
	}

	Outcome = FMeshImporter::Refinish(
		Mesh, Def->Finish, UMeshForgeSettings::Get()->NaniteTriangleThreshold);

	Def->LastImport = Outcome;
	Def->MarkPackageDirty();

	for (const FString& Warning : Outcome.Warnings)
	{
		UE_LOG(LogMeshForge, Warning, TEXT("'%s': %s"), *Def->GetName(), *Warning);
	}

	return Outcome;
}

FMeshImportOutcome UMeshForgeSubsystem::ImportMeshFile(
	const FString& AbsoluteFilePath,
	const FString& AssetName,
	const FMeshFinishSettings& Finish)
{
	FMeshImportOutcome Outcome;

	if (!FPaths::FileExists(AbsoluteFilePath))
	{
		Outcome.Error = FString::Printf(TEXT("No file at '%s'."), *AbsoluteFilePath);
		return Outcome;
	}

	const FString Extension = FPaths::GetExtension(AbsoluteFilePath).ToLower();

	if (Extension != TEXT("glb") && Extension != TEXT("gltf") && Extension != TEXT("fbx"))
	{
		// Said plainly rather than handed to Interchange to fail on. An FBX would import perfectly
		// well and then finish wrongly, because everything below assumes a mesh normalised into a
		// unit box - which an FBX from a DCC package is not.
		// FBX is admitted for one documented reason: quad topology arrives that way from at least
		// one generator, and refusing it meant refusing quad meshes. The original objection stands
		// for an FBX out of a DCC package - arbitrary scale, arbitrary axes - and this path cannot
		// tell the two apart, so the finish settings are what protect it either way.
		Outcome.Error = FString::Printf(
			TEXT("'%s' is not a mesh MeshForge finishes. Generated meshes arrive as .glb, .gltf or "
				 "- for quad topology - .fbx."),
			*AbsoluteFilePath);
		return Outcome;
	}

	const UMeshForgeSettings* Settings = UMeshForgeSettings::Get();

	FMeshImportRequest Request;
	Request.AbsoluteArtifactPath = AbsoluteFilePath;
	Request.MeshPackagePath = Settings->GetMeshesPath();
	Request.Finish = Finish;
	Request.NaniteTriangleThreshold = Settings->NaniteTriangleThreshold;
	Request.bSaveAssets = Settings->bSaveGeneratedAssets;

	Request.AssetName = AssetName.IsEmpty()
		? FString::Printf(TEXT("SM_%s"), *FPaths::GetBaseFilename(AbsoluteFilePath))
		: AssetName;

	Outcome = FMeshImporter::Import(Request);

	for (const FString& Warning : Outcome.Warnings)
	{
		UE_LOG(LogMeshForge, Warning, TEXT("'%s': %s"), *Request.AssetName, *Warning);
	}

	if (!Outcome.bSuccess)
	{
		UE_LOG(LogMeshForge, Error, TEXT("Importing '%s' failed: %s"),
			*AbsoluteFilePath, *Outcome.Error);
	}

	return Outcome;
}

#undef LOCTEXT_NAMESPACE

// -------------------------------------------------------------------------------------------------
// Background work
// -------------------------------------------------------------------------------------------------

FGuid UMeshForgeSubsystem::StartConceptDraw(UMeshDef* Def, FString& OutError)
{
	OutError.Reset();

	if (Def == nullptr)
	{
		OutError = TEXT("No definition.");
		return FGuid();
	}

	if (IsStageRunning(Def, EMeshStage::Concept))
	{
		OutError = TEXT("This definition is already drawing. Wait for it to finish.");
		return FGuid();
	}

	UMeshImagePipeline* Pipeline = Def->ConceptPipeline;

	if (Pipeline == nullptr)
	{
		OutError = TEXT("No image pipeline chosen. Pick one on the Concept stage - Local draws on "
						"this machine's GPU for nothing, Meshy draws through your Meshy account for "
						"credits.");
		return FGuid();
	}

	if (!Pipeline->bEnabled)
	{
		OutError = TEXT("The concept pipeline is switched off.");
		return FGuid();
	}

	if (Def->Prompt.IsEmpty())
	{
		OutError = TEXT("Write a prompt before drawing a reference image.");
		return FGuid();
	}

	// Asked before anything is spent. On a metered pipeline a setting that contradicts itself is not
	// always refused by the vendor - one of them silently changes mode and doubles the bill - so
	// this is the difference between a warning and an invoice.
	if (const FString Complaint = Pipeline->Validate(); !Complaint.IsEmpty())
	{
		OutError = Complaint;
		return FGuid();
	}

	// Copied, and held with a strong pointer for the duration.
	//
	// The background thread reads these settings while the panel beside it can still edit them, and
	// a half-applied change is worse than either value. A copy also means the run is the one that
	// was configured when the button was pressed, which is what somebody expects when they press it
	// and immediately start trying the next idea.
	UMeshImagePipeline* Copy = DuplicateObject<UMeshImagePipeline>(Pipeline, GetTransientPackage());

	if (Copy == nullptr)
	{
		OutError = TEXT("Could not take a copy of the image pipeline.");
		return FGuid();
	}

	FMeshForgeJob Job;
	Job.Id              = FGuid::NewGuid();
	Job.Definition      = Def;
	Job.DefinitionName  = Def->GetName();
	Job.Stage           = EMeshStage::Concept;
	Job.Label           = TEXT("Drawing a concept image");
	Job.State           = EMeshForgeJobState::Running;
	Job.StartedUtc      = FDateTime::UtcNow();
	Job.StartedSeconds  = FPlatformTime::Seconds();

	Jobs.Add(Job);

	FMeshStageState& State = Def->Stages.FindOrAdd(EMeshStage::Concept);
	State.Status = EMeshStageStatus::Running;
	State.Error.Reset();

	OnJobsChanged.Broadcast();

	const FGuid JobId = Job.Id;
	const FString Prompt = Def->Prompt;

	// --- the pictures the pipeline may work from ---------------------------------------------------
	//
	// Read here, on the game thread, because a texture's pixels cannot be touched from a worker.
	// Offered rather than required: a pipeline that draws from words alone ignores them, and one
	// that edits - extract this object, render this sketch, give me four views of it - cannot work
	// without them. Before this they arrived empty and the editing half of every image pipeline was
	// unreachable.
	//
	// Only what the mesh stage would actually use, main view first. Encoding the whole gallery would
	// mean a definition with fifteen concepts paying for fifteen PNG encodes on every draw.
	TArray<TArray<uint8>> Inputs;

	auto AddImage = [&Inputs](const TSoftObjectPtr<UTexture2D>& Soft)
	{
		if (Soft.IsNull())
		{
			return;
		}

		if (UTexture2D* Texture = Soft.LoadSynchronous())
		{
			TArray<uint8> Png;
			FString Ignored;

			if (FMeshImageIngest::EncodePng(Texture, Png, Ignored))
			{
				Inputs.Add(MoveTemp(Png));
			}
		}
	};

	if (Pipeline->GetIO().bNeedsImages)
	{
		AddImage(Def->ResolveMainImage());

		for (const TSoftObjectPtr<UTexture2D>& View : Def->ExtraViews)
		{
			AddImage(View);
		}
	}

	// Rooted rather than held in a TStrongObjectPtr, and the difference matters here: that type
	// registers and unregisters with the garbage collector on destruction, which is not safe to do
	// from a worker thread - and a copy captured into a worker lambda is destroyed on exactly that
	// thread. Rooting is a flag on the object, set and cleared on the game thread at both ends.
	Copy->AddToRoot();

	Async(EAsyncExecution::ThreadPool, [JobId, Prompt, Copy, Inputs = MoveTemp(Inputs)]()
	{
		FMeshImageJob Work;
		Work.Prompt = Prompt;
		Work.InputImages = Inputs;

		FMeshImageDrawResult Drawn;
		Copy->Draw(Work, Drawn);

		// Back to the game thread before anything touches an asset. Creating a texture, marking a
		// package dirty and broadcasting to a panel are all game-thread work, and the only reason
		// the drawing itself could leave it is that it is nothing but waiting.
		//
		// The pictures are moved rather than copied - a multi-view call comes back with several
		// megabytes of PNG, and this lambda is copied on its way to the queue.
		AsyncTask(ENamedThreads::GameThread, [JobId, Drawn = MoveTemp(Drawn), Copy]()
		{
			// Cleared on every path out of here, including the two failures below.
			ON_SCOPE_EXIT { Copy->RemoveFromRoot(); };

			UMeshForgeSubsystem* Self = UMeshForgeSubsystem::Get();
			if (Self == nullptr)
			{
				// The editor is shutting down. The pictures are lost, which is the right trade
				// against touching the asset system on the way out.
				return;
			}

			const FMeshForgeJob* Job = Self->Jobs.FindByPredicate(
				[&JobId](const FMeshForgeJob& Candidate) { return Candidate.Id == JobId; });

			UMeshDef* Def = Job ? Job->Definition.Get() : nullptr;

			if (Def == nullptr)
			{
				// The definition was deleted or reloaded while this ran. Nothing to write it onto.
				Self->FinishJob(JobId, false, TEXT("The definition went away while it was drawing."));
				return;
			}

			FString Error;
			const bool bOk = Def->FinishConceptDraw(Drawn, Error);

			Self->FinishJob(JobId, bOk, Error);
		});
	});

	UE_LOG(LogMeshForge, Log, TEXT("%s: drawing a concept image in the background."), *Def->GetName());
	return JobId;
}

void UMeshForgeSubsystem::FinishJob(const FGuid& JobId, bool bSuccess, const FString& Error)
{
	FMeshForgeJob* Job = Jobs.FindByPredicate(
		[&JobId](const FMeshForgeJob& Candidate) { return Candidate.Id == JobId; });

	if (Job == nullptr)
	{
		return;
	}

	Job->State           = bSuccess ? EMeshForgeJobState::Succeeded : EMeshForgeJobState::Failed;
	Job->Error           = Error;
	Job->FinishedSeconds = FPlatformTime::Seconds();

	// A failed job must never leave its stage reading "running".
	//
	// The success paths write their own state - they know the hash the output was made with, which
	// this does not - but failure can arrive from places that never reach them: a provider that
	// could not be started, a submission that returned nothing. The stage was stuck on "running"
	// forever after exactly that, with a job beside it that had plainly stopped.
	if (!bSuccess)
	{
		if (UMeshDef* Def = Job->Definition.Get())
		{
			FMeshStageState& State = Def->Stages.FindOrAdd(Job->Stage);

			if (State.Status == EMeshStageStatus::Running)
			{
				State.Status     = EMeshStageStatus::Failed;
				State.Error      = Error;
				State.LastRunUtc = FDateTime::UtcNow();
			}
		}
	}

	if (bSuccess)
	{
		UE_LOG(LogMeshForge, Log, TEXT("%s: %s finished in %.1fs."),
			*Job->DefinitionName, *Job->Label.ToLower(), Job->Elapsed());
	}
	else
	{
		UE_LOG(LogMeshForge, Warning, TEXT("%s: %s failed after %.1fs - %s"),
			*Job->DefinitionName, *Job->Label.ToLower(), Job->Elapsed(), *Error);
	}

	// Older finished jobs are dropped once there are a few, so the list stays a picture of what is
	// happening rather than a log. A failed one is kept longer than a successful one would be worth
	// keeping, because its error is the only place the reason is written down for a person.
	int32 Finished = 0;

	for (int32 Index = Jobs.Num() - 1; Index >= 0; --Index)
	{
		if (Jobs[Index].State == EMeshForgeJobState::Running)
		{
			continue;
		}

		if (++Finished > 8)
		{
			Jobs.RemoveAt(Index);
		}
	}

	OnJobsChanged.Broadcast();
}

void UMeshForgeSubsystem::BeginMeshJob(UMeshDef* Def)
{
	if (Def == nullptr || IsStageRunning(Def, EMeshStage::Mesh))
	{
		return;
	}

	FMeshForgeJob Job;
	Job.Id             = FGuid::NewGuid();
	Job.Definition     = Def;
	Job.DefinitionName = Def->GetName();
	Job.Stage          = EMeshStage::Mesh;
	Job.Label          = TEXT("Generating a mesh");
	Job.State          = EMeshForgeJobState::Running;
	Job.StartedUtc     = FDateTime::UtcNow();
	Job.StartedSeconds = FPlatformTime::Seconds();

	Jobs.Add(Job);

	FMeshStageState& State = Def->Stages.FindOrAdd(EMeshStage::Mesh);
	State.Status = EMeshStageStatus::Running;
	State.Error.Reset();

	OnJobsChanged.Broadcast();
}

FGuid UMeshForgeSubsystem::StartMeshGeneration(UMeshDef* Def, FString& OutError)
{
	OutError.Reset();

	if (Def == nullptr)
	{
		OutError = TEXT("No definition.");
		return FGuid();
	}

	if (IsStageRunning(Def, EMeshStage::Mesh))
	{
		OutError = TEXT("This definition is already generating. Wait for it to finish.");
		return FGuid();
	}

	if (Def->IsBusy())
	{
		OutError = TEXT("Jobs are already in flight for this definition; submitting again would "
						"generate - and on a metered provider, pay for - the same thing twice.");
		return FGuid();
	}

	TSharedPtr<IMeshProvider> Provider = ResolveProvider(Def);

	if (!Provider.IsValid())
	{
		OutError = TEXT("No mesh provider. Choose a mesh pipeline, or set a provider in Project "
						"Settings.");
		return FGuid();
	}

	// Refused rather than run. A definition with a supplied mesh ignores whatever generation
	// produces - the post stage reads the source mesh first - so generating would be a paid call
	// whose result nothing ever looks at. That is worth stopping loudly.
	if (!Def->SourceMesh.IsNull())
	{
		OutError = FString::Printf(
			TEXT("'%s' supplies its own mesh ('%s'), so there is nothing to generate. Run "
				 "post-processing instead, or clear Source Mesh to generate."),
			*Def->GetName(),
			*FPackageName::ObjectPathToObjectName(Def->SourceMesh.ToString()));
		return FGuid();
	}

	if (!Def->HasInput())
	{
		OutError = TEXT("Nothing to generate from - this definition has neither a prompt nor a "
						"picture.");
		return FGuid();
	}

	// --- the pictures the model needs -------------------------------------------------------------
	//
	// **Refused here rather than discovered after the money is gone.** A model that reconstructs from
	// an image and is handed none produces something from nothing; a multi-view request with no extra
	// views is a multi-view request in name only, and comes back looking like a single-image result
	// nobody can explain.
	const FMeshProviderCaps Caps = Provider->GetCaps();
	const bool bTextOnly = Def->ResolveMainImage().IsNull() && Def->SourceImagePath.IsEmpty();

	if (bTextOnly && Caps.bRequiresImage)
	{
		OutError = FString::Printf(
			TEXT("'%s' reconstructs from a picture and none is chosen. Draw or add one in the "
				 "Images tab, then set it as the main image."),
			*Provider->GetDisplayName());
		return FGuid();
	}

	if (Def->MeshPipeline != nullptr && Def->MeshPipeline->GetMaxExtraViews() > 0
		&& Def->ExtraViews.Num() == 0)
	{
		OutError = TEXT("This generator is set to send several views and only the main picture is "
						"chosen. Give at least one more picture a view slot in the Images tab, or "
						"turn multiview_to_model off in the generator's settings.");
		return FGuid();
	}

	BeginMeshJob(Def);

	const FMeshForgeJob* Registered = Jobs.FindByPredicate([Def](const FMeshForgeJob& Job)
	{
		return Job.State == EMeshForgeJobState::Running
			&& Job.Stage == EMeshStage::Mesh
			&& Job.Definition.Get() == Def;
	});

	const FGuid JobId = Registered ? Registered->Id : FGuid();

	// The job's label says what is actually happening, because "generating" while a container boots
	// is a lie somebody will time.
	if (FMeshForgeJob* Live = Jobs.FindByPredicate(
			[&JobId](const FMeshForgeJob& Job) { return Job.Id == JobId; }))
	{
		Live->Label = TEXT("Preparing the generator");
	}

	TWeakObjectPtr<UMeshDef> WeakDef(Def);

	// Weak, not `this`. Bringing a container up takes minutes, and an editor closed in the middle of
	// it would fire this callback into a destroyed subsystem.
	TWeakObjectPtr<UMeshForgeSubsystem> WeakSelf(this);

	Provider->PrepareForWork([WeakSelf, WeakDef, JobId](bool bReady, const FString& Reason)
	{
		UMeshForgeSubsystem* Self = WeakSelf.Get();

		if (Self == nullptr)
		{
			return;
		}

		UMeshDef* Target = WeakDef.Get();

		if (Target == nullptr)
		{
			Self->FinishJob(JobId, false,
				TEXT("The definition went away while its generator was starting."));
			return;
		}

		if (!bReady)
		{
			Target->SetStatus(EMeshDefStatus::Failed, Reason);
			Self->FinishJob(JobId, false, Reason);
			return;
		}

		if (FMeshForgeJob* Live = Self->Jobs.FindByPredicate(
				[&JobId](const FMeshForgeJob& Job) { return Job.Id == JobId; }))
		{
			Live->Label = TEXT("Generating a mesh");
			Self->OnJobsChanged.Broadcast();
		}

		// From here the existing batch machinery owns it: submit, poll on the ticker, download,
		// import. HandleDefFinished closes the job out on every path that reaches a terminal state.
		const FMeshBatchSubmission Submission = Self->GenerateMeshes({ Target });

		if (Submission.Submitted == 0)
		{
			Self->FinishJob(JobId, false, Submission.LastError);
		}
	});

	return JobId;
}


// -------------------------------------------------------------------------------------------------
// Post-processing
//
// The one stage that runs on a mesh rather than producing one, and the only stage that is useful on
// a mesh this plugin did not generate. Everything here is arranged around that second case: a
// modular corridor somebody modelled has no take, no task id and no provider, and it still has to
// reach a vendor.
// -------------------------------------------------------------------------------------------------

bool UMeshForgeSubsystem::ResolvePostInput(
	UMeshDef* Def,
	TArray<uint8>& OutGlb,
	FName& OutProviderId,
	FString& OutTaskId,
	FString& OutError) const
{
	OutGlb.Reset();
	OutProviderId = NAME_None;
	OutTaskId.Reset();
	OutError.Reset();

	if (Def == nullptr)
	{
		OutError = TEXT("No definition.");
		return false;
	}

	const UMeshForgeSettings* Settings = UMeshForgeSettings::Get();
	const FString Staging = Settings->GetStagingDirectory();

	// --- a mesh somebody supplied -----------------------------------------------------------------
	//
	// First, and unconditionally. Where a definition has one it *is* the subject, and falling
	// through to a generated take because the export was awkward would silently work on the wrong
	// mesh.
	if (!Def->SourceMesh.IsNull())
	{
		UStaticMesh* Mesh = Def->SourceMesh.LoadSynchronous();

		if (Mesh == nullptr)
		{
			OutError = FString::Printf(
				TEXT("'%s' names a source mesh that could not be loaded (%s). It may have been "
					 "deleted or renamed."),
				*Def->GetName(), *Def->SourceMesh.ToString());
			return false;
		}

		const FString Path = Staging / FString::Printf(TEXT("%s_source.glb"),
			*ObjectTools::SanitizeObjectName(Def->GetName()));

		// Geometry only. A texturing service replaces every material it is sent, and on a vendor
		// that takes the model as a base64 data URI the baked textures would be most of the upload.
		FMeshExportOptions Options;
		Options.bIncludeTextures = false;

		const FMeshExportResult Exported = FMeshExporter::ToGlbBytes(Mesh, Path, OutGlb, Options);

		if (!Exported.bSuccess)
		{
			OutError = Exported.Error;
			return false;
		}

		for (const FString& Warning : Exported.Warnings)
		{
			UE_LOG(LogMeshForge, Warning, TEXT("'%s': %s"), *Def->GetName(), *Warning);
		}

		return true;
	}

	// --- a take this definition generated ---------------------------------------------------------
	const FMeshCandidate* Candidate = Def->FindSelectedCandidate();

	if (Candidate == nullptr || !Candidate->IsUsable())
	{
		for (const FMeshCandidate& Other : Def->Candidates)
		{
			if (Other.IsUsable())
			{
				Candidate = &Other;
				break;
			}
		}
	}

	if (Candidate == nullptr || !Candidate->IsUsable())
	{
		OutError = FString::Printf(
			TEXT("'%s' has no mesh to work on. Generate one, or set a Source Mesh to post-process a "
				 "mesh you already have."),
			*Def->GetName());
		return false;
	}

	// The file the vault holds, which is the copy whose hash the record claims - not the staging
	// copy, which is overwritten by the next take.
	const FString Artifact = Candidate->LocalArtifactPath;

	if (Artifact.IsEmpty() || !FPaths::FileExists(Artifact))
	{
		OutError = FString::Printf(
			TEXT("'%s' has a finished take but no file for it on this machine. Import it once - "
				 "that downloads it - then post-process."),
			*Def->GetName());
		return false;
	}

	if (!FFileHelper::LoadFileToArray(OutGlb, *Artifact))
	{
		OutError = FString::Printf(TEXT("Could not read '%s'."), *Artifact);
		return false;
	}

	// Carried so a vendor that still holds this task can be pointed at its own copy. It is the
	// difference between a retexture that uploads a hundred megabytes and one that uploads a string.
	OutProviderId = Candidate->ProviderId;
	OutTaskId     = Candidate->MeshId;

	return true;
}

FGuid UMeshForgeSubsystem::StartPostProcessing(UMeshDef* Def, FString& OutError)
{
	OutError.Reset();

	if (Def == nullptr)
	{
		OutError = TEXT("No definition.");
		return FGuid();
	}

	if (IsStageRunning(Def, EMeshStage::Post))
	{
		OutError = TEXT("This definition is already post-processing. Wait for it to finish.");
		return FGuid();
	}

	if (Def->IsBusy())
	{
		OutError = TEXT("Jobs are already in flight for this definition.");
		return FGuid();
	}

	// Only the enabled ones, in order. A step switched off is kept deliberately - comparing with and
	// without is the common case - so skipping it here is what that switch is for.
	TArray<UMeshPostPipeline*> Steps;

	for (const TObjectPtr<UMeshPostPipeline>& Step : Def->PostPipelines)
	{
		if (Step != nullptr && Step->bEnabled)
		{
			Steps.Add(Step);
		}
	}

	if (Steps.Num() == 0)
	{
		OutError = TEXT("No post-processing steps to run. Add one on the Post stage.");
		return FGuid();
	}

	// Asked before anything is spent, and before the export - which on a dense mesh is not free
	// either. A chain that fails at step three having paid for one and two is the expensive wrong.
	for (const UMeshPostPipeline* Step : Steps)
	{
		const FString Complaint = Step->Validate();

		if (!Complaint.IsEmpty())
		{
			OutError = Complaint;
			return FGuid();
		}
	}

	TArray<uint8> Glb;
	FName SourceProviderId;
	FString SourceTaskId;

	if (!ResolvePostInput(Def, Glb, SourceProviderId, SourceTaskId, OutError))
	{
		return FGuid();
	}

	// Copied for the same reason a concept draw copies its pipeline: the worker reads these settings
	// while the panel beside it can still edit them, and a half-applied change is worse than either
	// value.
	TArray<UMeshPostPipeline*> Copies;

	for (UMeshPostPipeline* Step : Steps)
	{
		UMeshPostPipeline* Copy =
			DuplicateObject<UMeshPostPipeline>(Step, GetTransientPackage());

		if (Copy == nullptr)
		{
			for (UMeshPostPipeline* Made : Copies)
			{
				Made->RemoveFromRoot();
			}

			OutError = TEXT("Could not take a copy of a post-processing step.");
			return FGuid();
		}

		// Rooted rather than held in a TStrongObjectPtr: that type unregisters with the collector on
		// destruction, and a copy captured into a worker lambda is destroyed on that worker thread.
		Copy->AddToRoot();
		Copies.Add(Copy);

		// Here, on the game thread, while assets can still be loaded and read. A step that paints
		// from a picture reads that picture now; Run cannot.
		FString PrepareError;

		if (!Copy->Prepare(PrepareError))
		{
			for (UMeshPostPipeline* Made : Copies)
			{
				Made->RemoveFromRoot();
			}

			OutError = PrepareError.IsEmpty()
				? FString(TEXT("A post-processing step could not read what it needs."))
				: PrepareError;

			return FGuid();
		}
	}

	FMeshForgeJob Job;
	Job.Id             = FGuid::NewGuid();
	Job.Definition     = Def;
	Job.DefinitionName = Def->GetName();
	Job.Stage          = EMeshStage::Post;
	Job.Label          = Steps.Num() == 1
		? FString(TEXT("Post-processing the mesh"))
		: FString::Printf(TEXT("Post-processing the mesh (%d steps)"), Steps.Num());
	Job.State          = EMeshForgeJobState::Running;
	Job.StartedUtc     = FDateTime::UtcNow();
	Job.StartedSeconds = FPlatformTime::Seconds();

	Jobs.Add(Job);

	FMeshStageState& State = Def->Stages.FindOrAdd(EMeshStage::Post);
	State.Status = EMeshStageStatus::Running;
	State.Error.Reset();

	OnJobsChanged.Broadcast();

	const FGuid JobId = Job.Id;
	const FString Name = Def->GetName();
	const FString Prompt = Def->Prompt;

	Async(EAsyncExecution::ThreadPool,
		[JobId, Name, Prompt, Copies, Glb = MoveTemp(Glb), SourceProviderId, SourceTaskId]()
	{
		FMeshPostJob Work;
		Work.MeshGlb          = Glb;
		Work.Name             = Name;
		Work.Prompt           = Prompt;
		Work.SourceProviderId = SourceProviderId;
		Work.SourceTaskId     = SourceTaskId;

		TArray<FMeshPostResult> Results;
		FString Error;

		for (UMeshPostPipeline* Step : Copies)
		{
			FMeshPostResult Result;
			Step->Run(Work, Result);

			if (!Result.IsOk())
			{
				// Stopped, not skipped. Carrying on would import a mesh missing a pass somebody paid
				// for, and it would look exactly like one that is not.
				Error = Result.Error.IsEmpty()
					? FString::Printf(TEXT("'%s' produced no mesh and gave no reason."),
						*Step->GetClass()->GetName())
					: Result.Error;

				Results.Add(MoveTemp(Result));
				break;
			}

			// The next step works on this one's output, and points at it rather than uploading it
			// again where the vendor is the same one.
			Work.MeshGlb          = Result.MeshGlb;
			Work.SourceProviderId = Step->GetProviderId();
			Work.SourceTaskId     = Result.TaskId;

			Results.Add(MoveTemp(Result));
		}

		TArray<uint8> Final = Error.IsEmpty() ? Work.MeshGlb : TArray<uint8>();

		AsyncTask(ENamedThreads::GameThread,
			[JobId, Copies, Final = MoveTemp(Final), Results = MoveTemp(Results), Error]()
		{
			// Cleared on every path out, including the failure below.
			ON_SCOPE_EXIT
			{
				for (UMeshPostPipeline* Copy : Copies)
				{
					Copy->RemoveFromRoot();
				}
			};

			UMeshForgeSubsystem* Self = UMeshForgeSubsystem::Get();

			if (Self == nullptr)
			{
				// The editor is shutting down. Touching the asset system on the way out is worse
				// than losing this.
				return;
			}

			// **Deferred by one tick rather than finished here, and an editor crash is the reason.**
			// Importing runs Interchange, which pumps the game thread's task queue while it waits -
			// and pumping the task graph from *inside* a task-graph task is re-entrant. It asserts
			// on `++Queue(QueueIndex).RecursionGuard == 1` and takes the editor with it. Observed
			// 2026-08-31 on the first retexture: the mesh came back, was filed, and the import killed
			// the process.
			//
			// Generation never hit this because it imports from the Poll ticker, which is outside
			// the task graph. A one-shot ticker puts this on the same footing.
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[JobId, Final, Results, Error](float) -> bool
				{
					if (UMeshForgeSubsystem* Live = UMeshForgeSubsystem::Get())
					{
						Live->FinishPostRun(JobId, Final, Results, Error);
					}

					return false;   // once
				}), 0.0f);
		});
	});

	UE_LOG(LogMeshForge, Log, TEXT("'%s': running %d post-processing step(s) in the background."),
		*Def->GetName(), Steps.Num());

	return JobId;
}

void UMeshForgeSubsystem::FinishPostRun(
	const FGuid& JobId,
	const TArray<uint8>& Glb,
	const TArray<FMeshPostResult>& Steps,
	const FString& Error)
{
	check(IsInGameThread());

	const FMeshForgeJob* Job = Jobs.FindByPredicate(
		[&JobId](const FMeshForgeJob& Candidate) { return Candidate.Id == JobId; });

	UMeshDef* Def = Job ? Job->Definition.Get() : nullptr;

	if (Def == nullptr)
	{
		FinishJob(JobId, false, TEXT("The definition went away while it was post-processing."));
		return;
	}

	const FDateTime StartedUtc = Job->StartedUtc;

	FMeshStageState& Stage = Def->Stages.FindOrAdd(EMeshStage::Post);
	Stage.LastRunUtc = FDateTime::UtcNow();

	float Spent = 0.0f;
	int32 Credits = -1;

	for (const FMeshPostResult& Step : Steps)
	{
		Spent += Step.Seconds;

		if (Step.ConsumedCredits >= 0)
		{
			Credits = FMath::Max(0, Credits) + Step.ConsumedCredits;
		}
	}

	Stage.LastRunSeconds = Spent;

	if (!Error.IsEmpty() || Glb.Num() == 0)
	{
		const FString Reason = Error.IsEmpty()
			? FString(TEXT("Post-processing finished with no mesh."))
			: Error;

		Stage.Status = EMeshStageStatus::Failed;
		Stage.Error  = Reason;

		Def->SetStatus(EMeshDefStatus::Failed, Reason);
		FinishJob(JobId, false, Reason);
		return;
	}

	const UMeshForgeSettings* Settings = UMeshForgeSettings::Get();
	const FString Staging = Settings->GetStagingDirectory();
	IFileManager::Get().MakeDirectory(*Staging, true);

	// The last step's own handle where it has one, so the file, the record and the vendor's task all
	// carry the same name. A local step that has no handle gets a fresh id rather than sharing the
	// input's, which would overwrite the take it was made from.
	FString TakeId = Steps.Num() > 0 ? Steps.Last().TaskId : FString();

	if (TakeId.IsEmpty())
	{
		TakeId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	const FString ArtifactPath = Staging / FString::Printf(TEXT("%s.glb"),
		*ObjectTools::SanitizeObjectName(TakeId));

	if (!FFileHelper::SaveArrayToFile(Glb, *ArtifactPath))
	{
		const FString Reason = FString::Printf(
			TEXT("Post-processing finished but the result could not be written to '%s'."),
			*ArtifactPath);

		Stage.Status = EMeshStageStatus::Failed;
		Stage.Error  = Reason;

		Def->SetStatus(EMeshDefStatus::Failed, Reason);
		FinishJob(JobId, false, Reason);
		return;
	}

	// --- filed before anything is imported ---------------------------------------------------------
	//
	// Same rule as a generated take, and for the same reason: an import can fail, an editor can be
	// closed, a definition can be deleted, and none of that may lose something that was paid for.
	FString VaultArtifact = ArtifactPath;

	{
		FForgeTakeRecord Record;

		Record.TakeId = TakeId;
		Record.Plugin = TEXT("MeshForge");
		Record.Kind   = TEXT("mesh-post");

		Record.DefinitionPath = Def->GetPathName();
		Record.DefinitionName = Def->GetName();

		Record.Prompt = Def->Prompt;

		// What went in. Named as the asset where somebody supplied one, as the take it came from
		// where the pipeline made it - a record that cannot say which is a record nobody can audit.
		if (!Def->SourceMesh.IsNull())
		{
			FForgeTakeInput Input;
			Input.Role      = TEXT("source-mesh");
			Input.AssetPath = Def->SourceMesh.ToString();
			Record.Inputs.Add(Input);
		}
		else if (const FMeshCandidate* From = Def->FindSelectedCandidate())
		{
			FForgeTakeInput Input;
			Input.Role = TEXT("source-take");
			Input.File = From->LocalArtifactPath;
			Input.Hash = FForgeLibrary::HashFile(From->LocalArtifactPath);
			Record.Inputs.Add(Input);

			Record.Settings.Add(TEXT("sourceTakeId"), From->MeshId);
			Record.Settings.Add(TEXT("sourceProviderId"), From->ProviderId.ToString());
		}

		// Every step, in order, with what it actually did. The chain is the provenance here: one
		// name would be a lie the moment a second step is added.
		for (int32 Index = 0; Index < Def->PostPipelines.Num(); ++Index)
		{
			const UMeshPostPipeline* Step = Def->PostPipelines[Index];

			if (Step == nullptr || !Step->bEnabled)
			{
				continue;
			}

			Record.Settings.Add(
				FString::Printf(TEXT("step%d"), Index + 1),
				FString::Printf(TEXT("%s %s"), *Step->GetClass()->GetName(), *Step->Signature()));

			if (Record.ProviderId.IsEmpty())
			{
				Record.ProviderId = Step->GetProviderId().ToString();
			}
		}

		for (const FMeshPostResult& Step : Steps)
		{
			if (Step.Summary.IsEmpty())
			{
				continue;
			}

			FForgeProcessingEvent Event;
			Event.StepId  = TEXT("meshforge.post");
			Event.Tool    = TEXT("MeshForge");
			Event.AtUtc   = FDateTime::UtcNow();
			Event.Summary = Step.Summary;

			Record.Processing.Add(Event);
		}

		if (Credits >= 0)
		{
			// The vendor's own figure, so this take can be reconciled against an invoice rather than
			// only argued with.
			Record.CostCurrency   = TEXT("credits");
			Record.CostAmount     = Credits;
			Record.bCostEstimated = false;
		}

		Record.StartedUtc  = StartedUtc;
		Record.FinishedUtc = FDateTime::UtcNow();
		Record.Seconds     = Spent;
		Record.ToolVersion = TEXT("MeshForge");

		FString VaultError;

		if (FForgeLibrary::Deposit(Record, ArtifactPath, VaultError))
		{
			// Imported from the vault copy, so what lands in the project is demonstrably the bytes
			// the record hashed.
			VaultArtifact = Record.ArtifactPath();
		}
		else
		{
			// Not fatal. A pass that could not be filed is still a pass that can be imported, and
			// refusing here would throw away the expensive thing to protect the record of it.
			UE_LOG(LogMeshForge, Warning,
				TEXT("'%s': could not file the post-processed mesh in the library - %s"),
				*Def->GetName(), *VaultError);
		}
	}

	Stage.Status     = EMeshStageStatus::Ready;
	Stage.InputsHash = Def->ComputeStageHash(EMeshStage::Post);
	Stage.Error.Reset();

	// --- and into the project ----------------------------------------------------------------------
	const double ImportStarted = FPlatformTime::Seconds();

	FMeshImportRequest Request;
	Request.AbsoluteArtifactPath    = VaultArtifact;

	// The post take's own folder, for the same reason the mesh stage uses one - and here it mattered
	// most, because a retexture reuses the *source* mesh's material name and so collided every time.
	Request.MeshPackagePath         = Settings->GetPostTakeFolder(Def->GetName(), TakeId);
	Request.Finish                  = Def->Finish;
	Request.NaniteTriangleThreshold = Settings->NaniteTriangleThreshold;
	Request.bSaveAssets             = Settings->bSaveGeneratedAssets;

	// The same name a generated take would import under, so post-processing replaces the definition's
	// mesh rather than leaving two that differ by a suffix nobody can read.
	Request.AssetName = FString::Printf(TEXT("SM_%s"),
		*Def->GetName().Replace(TEXT("MSD_"), TEXT(""), ESearchCase::CaseSensitive));

	const FMeshImportOutcome Outcome = FMeshImporter::Import(Request);

	Def->LastImport = Outcome;

	FMeshStageState& ImportStage = Def->Stages.FindOrAdd(EMeshStage::Import);
	ImportStage.LastRunUtc     = FDateTime::UtcNow();
	ImportStage.LastRunSeconds = static_cast<float>(FPlatformTime::Seconds() - ImportStarted);

	if (Outcome.bSuccess)
	{
		Def->ImportedMesh = Outcome.Mesh;
		Def->SetStatus(EMeshDefStatus::Imported);

		ImportStage.Status     = EMeshStageStatus::Ready;
		ImportStage.InputsHash = Def->ComputeStageHash(EMeshStage::Import);
		ImportStage.Error.Reset();

		for (const FString& Warning : Outcome.Warnings)
		{
			UE_LOG(LogMeshForge, Warning, TEXT("'%s': %s"), *Def->GetName(), *Warning);
		}

		FForgeProcessingEvent Event;
		Event.StepId  = TEXT("unreal.import");
		Event.Tool    = TEXT("MeshForge");
		Event.AtUtc   = FDateTime::UtcNow();
		Event.Summary = FString::Printf(
			TEXT("%d triangles, %d LODs, %d collision primitive(s), Nanite %s"),
			Outcome.TriangleCount, Outcome.LodCount, Outcome.CollisionPrimitives,
			Outcome.bNaniteEnabled ? TEXT("on") : TEXT("off"));

		FString AppendError;
		FForgeLibrary::AppendProcessing(
			FForgeLibrary::TakeDirectory(TEXT("MeshForge"), Def->GetName(), TakeId),
			Event, AppendError);
	}
	else
	{
		Def->SetStatus(EMeshDefStatus::Failed, Outcome.Error);

		ImportStage.Status = EMeshStageStatus::Failed;
		ImportStage.Error  = Outcome.Error;
	}

	Def->MarkPackageDirty();

	FinishJob(JobId, Outcome.bSuccess, Outcome.Error);
}

void UMeshForgeSubsystem::FileFailedTakes(UMeshDef* Def)
{
	if (Def == nullptr)
	{
		return;
	}

	// **A failed generation is still a spend, and a library that lists only successes understates
	// what a definition cost.** The provider accepted the job, may well have charged for it, and
	// reports consumed credits either way - so the record is written with no artifact beside it.
	// Somebody reading the ledger to find out what this prop cost gets the truth rather than the
	// happy path.
	for (const FMeshCandidate& Candidate : Def->Candidates)
	{
		if (Candidate.Status != EMeshJobStatus::Failed && Candidate.Status != EMeshJobStatus::Cancelled)
		{
			continue;
		}

		const FString Id = Candidate.MeshId.IsEmpty() ? Candidate.JobId : Candidate.MeshId;

		if (Id.IsEmpty())
		{
			// Nothing to key a record on - the provider never got far enough to name it.
			continue;
		}

		const FString Directory = FForgeLibrary::TakeDirectory(TEXT("MeshForge"), Def->GetName(), Id);

		FForgeTakeRecord Existing;

		if (FForgeLibrary::ReadRecord(Directory, Existing))
		{
			continue;   // already filed, and rewriting it would lose whatever was appended since
		}

		FForgeTakeRecord Record = BuildTakeRecord(Def, Candidate);
		Record.TakeId = Id;
		Record.Status = TEXT("failed");
		Record.Error  = Candidate.Error.IsEmpty()
			? FString(TEXT("The provider reported this take as failed and gave no reason."))
			: Candidate.Error;

		FString WriteError;

		if (!FForgeLibrary::WriteRecord(Record, WriteError))
		{
			UE_LOG(LogMeshForge, Warning,
				TEXT("'%s': could not record failed take %s - %s"),
				*Def->GetName(), *Id, *WriteError);
		}
	}
}

void UMeshForgeSubsystem::HandleDefFinished(UMeshDef* Def, bool bSuccess)
{
	// Written whether this succeeded or not, because the failures are the half nobody records and
	// the half that still cost money.
	FileFailedTakes(Def);

	if (Def == nullptr)
	{
		return;
	}

	FMeshStageState& Mesh = Def->Stages.FindOrAdd(EMeshStage::Mesh);

	// How long it took, taken from the job that was tracking it. The stage recorded zero seconds
	// for every mesh and every import, so the panel could never say how long anything had taken -
	// which is the number somebody wants when deciding whether to raise the quality step.
	float Elapsed = 0.0f;

	for (const FMeshForgeJob& Job : Jobs)
	{
		if (Job.Stage == EMeshStage::Mesh && Job.Definition.Get() == Def)
		{
			Elapsed = Job.Elapsed();
			break;
		}
	}

	if (bSuccess)
	{
		Mesh.Status         = EMeshStageStatus::Ready;
		Mesh.LastRunUtc     = FDateTime::UtcNow();
		Mesh.LastRunSeconds = Elapsed;
		Mesh.InputsHash     = Def->ComputeStageHash(EMeshStage::Mesh);
		Mesh.Error.Reset();

		// The Import stage is not touched here. It is written by ImportCandidate, which is the only
		// place that knows an import actually ran.
	}
	else
	{
		Mesh.Status         = EMeshStageStatus::Failed;
		Mesh.Error          = Def->LastError;
		Mesh.LastRunUtc     = FDateTime::UtcNow();
		Mesh.LastRunSeconds = Elapsed;
	}

	// Whichever job was tracking it, if any. A definition generated through the agent tools has no
	// job registered, and finishing nothing is the right outcome there.
	for (const FMeshForgeJob& Job : Jobs)
	{
		if (Job.State == EMeshForgeJobState::Running
			&& Job.Stage == EMeshStage::Mesh
			&& Job.Definition.Get() == Def)
		{
			FinishJob(Job.Id, bSuccess, Def->LastError);
			break;
		}
	}

	OnJobsChanged.Broadcast();
}

bool UMeshForgeSubsystem::FindJob(const FGuid& JobId, FMeshForgeJob& OutJob) const
{
	if (const FMeshForgeJob* Found = Jobs.FindByPredicate(
			[&JobId](const FMeshForgeJob& Job) { return Job.Id == JobId; }))
	{
		OutJob = *Found;
		return true;
	}

	return false;
}

TArray<FMeshForgeJob> UMeshForgeSubsystem::GetJobs() const
{
	TArray<FMeshForgeJob> Sorted = Jobs;

	// Running first, then most recent. Somebody looking at this list is nearly always asking "is it
	// still going", and the answer should not be below three things that have already finished.
	Sorted.Sort([](const FMeshForgeJob& A, const FMeshForgeJob& B)
	{
		const bool bARunning = (A.State == EMeshForgeJobState::Running);
		const bool bBRunning = (B.State == EMeshForgeJobState::Running);

		if (bARunning != bBRunning)
		{
			return bARunning;
		}

		return A.StartedSeconds > B.StartedSeconds;
	});

	return Sorted;
}

bool UMeshForgeSubsystem::IsStageRunning(const UMeshDef* Def, EMeshStage Stage) const
{
	if (Def == nullptr)
	{
		return false;
	}

	return Jobs.ContainsByPredicate([Def, Stage](const FMeshForgeJob& Job)
	{
		return Job.State == EMeshForgeJobState::Running
			&& Job.Stage == Stage
			&& Job.Definition.Get() == Def;
	});
}

float UMeshForgeSubsystem::StageElapsed(const UMeshDef* Def, EMeshStage Stage) const
{
	if (Def == nullptr)
	{
		return 0.0f;
	}

	const FMeshForgeJob* Job = Jobs.FindByPredicate([Def, Stage](const FMeshForgeJob& Candidate)
	{
		return Candidate.State == EMeshForgeJobState::Running
			&& Candidate.Stage == Stage
			&& Candidate.Definition.Get() == Def;
	});

	return Job ? Job->Elapsed() : 0.0f;
}

void UMeshForgeSubsystem::ClearFinishedJobs()
{
	const int32 Removed = Jobs.RemoveAll([](const FMeshForgeJob& Job)
	{
		return Job.State != EMeshForgeJobState::Running;
	});

	if (Removed > 0)
	{
		OnJobsChanged.Broadcast();
	}
}

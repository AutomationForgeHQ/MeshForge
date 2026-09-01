#include "MeshDef.h"

#include "MeshForge.h"
#include "MeshForgeSettings.h"
#include "MeshForgeSubsystem.h"
#include "IMeshProvider.h"
#include "MeshImageIngest.h"
#include "MeshForgePipeline.h"
#include "MeshExporter.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "ScopedTransaction.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "MeshForge"

namespace MeshDefStages
{
	/** A pipeline's contribution to a hash, or a marker saying there wasn't one. */
	static FString Describe(const UMeshForgePipeline* Pipeline)
	{
		if (Pipeline == nullptr)  { return TEXT("none|"); }
		if (!Pipeline->bEnabled)  { return TEXT("off|"); }
		return Pipeline->Signature() + TEXT("|");
	}

	/**
	 * Templated over the element, because the stages hold arrays of different pipeline types and
	 * one overload per type is a line somebody forgets when a fourth stage gains a list - leaving
	 * that stage's settings silently outside the hash, which is a definition claiming an output it
	 * no longer produces.
	 */
	template <typename PipelineType>
	static FString Describe(const TArray<TObjectPtr<PipelineType>>& Pipelines)
	{
		FString Out;
		for (const TObjectPtr<PipelineType>& Pipeline : Pipelines)
		{
			Out += Describe(static_cast<const UMeshForgePipeline*>(Pipeline.Get()));
		}
		return Out.IsEmpty() ? TEXT("empty|") : Out;
	}

	/** Order matters and is preserved: reordering a chain changes what it produces. */
	static FString Describe(const TArray<TSoftObjectPtr<UTexture2D>>& Images)
	{
		FString Out;
		for (const TSoftObjectPtr<UTexture2D>& Image : Images)
		{
			Out += Image.ToString() + TEXT(",");
		}
		return Out;
	}
}

FMeshStageState UMeshDef::GetStageState(EMeshStage Stage) const
{
	if (const FMeshStageState* Found = Stages.Find(Stage))
	{
		return *Found;
	}
	return FMeshStageState();
}

FString UMeshDef::ComputeStageHash(EMeshStage Stage) const
{
	// Each stage's hash includes every stage before it. That is what makes editing the prompt mark
	// the *mesh* stale rather than only the concept image - the mesh was made from a picture that
	// no longer exists, and saying so is the whole point of this.
	FString Input;

	Input += TEXT("prompt=") + Prompt + TEXT("|");
	Input += TEXT("concept=") + MeshDefStages::Describe(ConceptPipeline) ;

	if (Stage == EMeshStage::Concept)
	{
		return FMD5::HashAnsiString(*Input);
	}

	Input += TEXT("refine=") + MeshDefStages::Describe(RefinementPipelines);
	Input += TEXT("srcimg=") + SourceImage.ToString() + TEXT("|");
	Input += TEXT("srcpath=") + SourceImagePath + TEXT("|");
	Input += TEXT("concepts=") + MeshDefStages::Describe(ConceptImages);
	Input += TEXT("added=") + MeshDefStages::Describe(AddedImages);
	Input += TEXT("views=") + MeshDefStages::Describe(ExtraViews);
	Input += TEXT("main=") + ResolveMainImage().ToString() + TEXT("|");

	if (Stage == EMeshStage::References)
	{
		return FMD5::HashAnsiString(*Input);
	}

	// First, because it overrides everything above it: a definition that supplies its own mesh
	// is not made from the pictures the stages before it drew.
	Input += TEXT("srcmesh=") + SourceMesh.ToString() + TEXT("|");
	Input += TEXT("meshpipe=") + MeshDefStages::Describe(MeshPipeline);
	Input += TEXT("provider=") + ProviderId.ToString() + TEXT("|");
	Input += TEXT("model=") + ModelId + TEXT("|");
	Input += FString::Printf(TEXT("quality=%d|variants=%d|seed=%d|useseed=%d|"),
		static_cast<int32>(Control.Quality), Variants, Control.Seed, Control.bUseSeed ? 1 : 0);
	Input += TEXT("refs=") + MeshDefStages::Describe(ReferenceImages);

	if (Stage == EMeshStage::Mesh)
	{
		return FMD5::HashAnsiString(*Input);
	}

	Input += TEXT("post=") + MeshDefStages::Describe(PostPipelines);
	Input += TEXT("selectedmesh=") + SelectedMeshId + TEXT("|");

	if (Stage == EMeshStage::Post)
	{
		return FMD5::HashAnsiString(*Input);
	}

	// Import. Reflected rather than listed field by field, so a finish setting added later is
	// covered without anybody remembering to add it here - the failure mode being a definition that
	// claims to be imported with collision it does not have.
	if (const UScriptStruct* FinishStruct = FMeshFinishSettings::StaticStruct())
	{
		FString FinishText;
		FinishStruct->ExportText(FinishText, &Finish, nullptr, nullptr, PPF_None, nullptr);
		Input += TEXT("finish=") + FinishText;
	}

	return FMD5::HashAnsiString(*Input);
}

void UMeshDef::RefreshStaleness()
{
	static const EMeshStage Order[] = {
		EMeshStage::Concept, EMeshStage::References, EMeshStage::Mesh,
		EMeshStage::Post,    EMeshStage::Import,
	};

	bool bUpstreamDrifted = false;

	for (const EMeshStage Stage : Order)
	{
		FMeshStageState* State = Stages.Find(Stage);
		if (State == nullptr || !State->HasOutput())
		{
			// A stage with nothing to show cannot be stale, but it does not stop the drift either:
			// an empty refinement stage between a re-drawn concept and an existing mesh still means
			// that mesh came from a different picture.
			continue;
		}

		const bool bDrifted = bUpstreamDrifted || State->InputsHash != ComputeStageHash(Stage);

		State->Status = bDrifted ? EMeshStageStatus::Stale : EMeshStageStatus::Ready;
		bUpstreamDrifted |= bDrifted;
	}
}

const FMeshCandidate* UMeshDef::FindSelectedCandidate() const
{
	return SelectedMeshId.IsEmpty() ? nullptr : FindCandidate(SelectedMeshId);
}

const FMeshCandidate* UMeshDef::FindCandidate(const FString& MeshId) const
{
	return Candidates.FindByPredicate(
		[&MeshId](const FMeshCandidate& Candidate) { return Candidate.MeshId == MeshId; });
}

FMeshCandidate* UMeshDef::FindCandidateMutable(const FString& MeshId)
{
	return Candidates.FindByPredicate(
		[&MeshId](const FMeshCandidate& Candidate) { return Candidate.MeshId == MeshId; });
}

FMeshCandidate* UMeshDef::FindCandidateByJobMutable(const FString& JobId)
{
	return Candidates.FindByPredicate(
		[&JobId](const FMeshCandidate& Candidate) { return Candidate.JobId == JobId; });
}

bool UMeshDef::IsBusy() const
{
	if (Status == EMeshDefStatus::Submitting || Status == EMeshDefStatus::Generating)
	{
		return true;
	}

	return Candidates.ContainsByPredicate([](const FMeshCandidate& Candidate)
	{
		return Candidate.Status == EMeshJobStatus::Pending || Candidate.Status == EMeshJobStatus::Running;
	});
}

int32 UMeshDef::CountUsableCandidates() const
{
	int32 Count = 0;

	for (const FMeshCandidate& Candidate : Candidates)
	{
		Count += Candidate.IsUsable() ? 1 : 0;
	}

	return Count;
}

bool UMeshDef::HasInput() const
{
	// A supplied mesh counts, and has to: a definition made purely to retexture a corridor has no
	// prompt and no picture, and is nonetheless complete. Without this it reads as empty and every
	// stage that guards on it refuses.
	return !SourceMesh.IsNull()
		|| !Prompt.IsEmpty()
		|| !ResolveMainImage().IsNull()
		|| !SourceImagePath.IsEmpty();
}

void UMeshDef::ApplySpec(const FMeshDefSpec& Spec)
{
	Prompt = Spec.Prompt;
	SourceImagePath = Spec.SourceImagePath;
	Variants = FMath::Clamp(Spec.Variants, 1, 16);
	Control = Spec.Control;
	Finish = Spec.Finish;

	// Only overwrite the routing fields when the spec actually named something. An agent filling in
	// a prompt and nothing else should not silently clear a provider somebody chose by hand.
	if (!Spec.ProviderId.IsNone())
	{
		ProviderId = Spec.ProviderId;
	}

	if (!Spec.ModelId.IsEmpty())
	{
		ModelId = Spec.ModelId;
	}

	ApplyProjectDefaults();
}

void UMeshDef::ApplyProjectDefaults()
{
	const UMeshForgeSettings* Settings = GetDefault<UMeshForgeSettings>();

	if (ProviderId.IsNone())
	{
		ProviderId = Settings->DefaultProviderId;
	}

	// The model belongs to the provider, so a default is only meaningful when the provider matches.
	// Copying it across providers would name a Meshy model on a TRELLIS definition, which fails at
	// submit time with a message about an unknown model rather than about the real mistake.
	if (ModelId.IsEmpty() && ProviderId == Settings->DefaultProviderId)
	{
		ModelId = Settings->DefaultModelId;
	}

	// The finish settings are deliberately *not* touched here.
	//
	// There is no sentinel that distinguishes "nobody chose this" from "somebody chose the value
	// that happens to be the default", and every attempt to invent one was a check that never
	// fired: a fresh struct has a scale of 100 and a lightmap resolution of 128, so guarding on
	// either being zero simply never ran, and the project's default finish quietly did nothing.
	//
	// So the project default is applied where it can be applied honestly - at creation, by whoever
	// creates the asset, before anybody has had the chance to choose. The factory does it for an
	// asset made in the Content Browser; a caller passing a spec has already filled every field,
	// because the toolset schema requires them all.
}

void UMeshDef::SetStatus(EMeshDefStatus NewStatus, const FString& Error)
{
	Status = NewStatus;

	if (NewStatus == EMeshDefStatus::Failed)
	{
		LastError = Error;
		UE_LOG(LogMeshForge, Warning, TEXT("'%s' failed: %s"), *GetName(), *Error);
	}
	else if (Error.IsEmpty())
	{
		LastError.Empty();
	}

	MarkPackageDirty();
}

namespace MeshDefButtons
{
	/** A toast, because a button whose only trace is the Output Log looks like a button that did nothing. */
	static void Toast(const FText& Message, bool bSuccess)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = bSuccess ? 6.0f : 10.0f;
		Info.bFireAndForget = true;
		Info.bUseSuccessFailIcons = true;

		if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}
}

void UMeshDef::GenerateNow()
{
	UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get();

	if (Subsystem == nullptr)
	{
		MeshDefButtons::Toast(LOCTEXT("NoSubsystem", "MeshForge is not available."), false);
		return;
	}

	// Started, not run - and started even when the generator's container is stopped, which this
	// takes minutes to fix rather than refusing over. Progress is on the stage and in the job list.
	FString Error;

	if (!Subsystem->StartMeshGeneration(this, Error).IsValid())
	{
		MeshDefButtons::Toast(FText::FromString(Error), false);
		SetStatus(EMeshDefStatus::Failed, Error);
		return;
	}

	MeshDefButtons::Toast(FText::Format(
		LOCTEXT("Submitted", "{0}: generating. The mesh appears here when it finishes."),
		FText::FromString(GetName())), true);
}

void UMeshDef::DrawConceptImage()
{
	UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get();

	if (Subsystem == nullptr)
	{
		SetStatus(EMeshDefStatus::Failed, TEXT("MeshForge is not running."));
		return;
	}

	// Started, not run. Everything that could go wrong before the work begins - no pipeline, no
	// prompt, settings that contradict each other - is decided here and reported now; everything
	// that can go wrong during it lands on the stage and in the job list.
	FString Error;

	if (!Subsystem->StartConceptDraw(this, Error).IsValid())
	{
		SetStatus(EMeshDefStatus::Failed, Error);
	}
}

bool UMeshDef::FinishConceptDraw(const FMeshImageDrawResult& Drawn, FString& OutError)
{
	check(IsInGameThread());

	FMeshStageState& State = Stages.FindOrAdd(EMeshStage::Concept);

	if (!Drawn.IsOk())
	{
		State.Status = EMeshStageStatus::Failed;
		State.Error  = Drawn.Error.IsEmpty()
			? TEXT("The image pipeline returned no pictures and no reason.")
			: Drawn.Error;

		OutError = State.Error;
		SetStatus(EMeshDefStatus::Failed, State.Error);
		return false;
	}

	const FScopedTransaction Transaction(
		NSLOCTEXT("MeshForge", "DrawConcept", "Draw concept image"));
	Modify();

	const FString Folder = FMeshImageIngest::ImageFolderFor(this);
	UTexture2D* First = nullptr;
	FString IngestError;

	// Filled only for a declared view set, and used to claim the extra view slots below.
	TArray<TSoftObjectPtr<UTexture2D>> Views;

	for (const FMeshDrawnImage& Image : Drawn.Images)
	{
		const FString BaseName = Image.Suffix.IsEmpty()
			? FString::Printf(TEXT("T_%s_Concept"), *GetName())
			: FString::Printf(TEXT("T_%s_Concept_%s"), *GetName(), *Image.Suffix);

		UTexture2D* Texture =
			FMeshImageIngest::CreateTextureAsset(Image.Bytes, Folder, BaseName, IngestError);

		if (Texture == nullptr)
		{
			// One picture failing to ingest does not discard the others - on a metered pipeline
			// every one of them has already been paid for.
			UE_LOG(LogMeshForge, Warning, TEXT("%s: could not ingest a drawn image: %s"),
				*GetName(), *IngestError);
			continue;
		}

		ConceptImages.Add(Texture);

		if (First == nullptr) { First = Texture; }
		else if (Drawn.bIsViewSet) { Views.Add(Texture); }
	}

	if (First == nullptr)
	{
		State.Status = EMeshStageStatus::Failed;
		State.Error  = IngestError.IsEmpty()
			? TEXT("The pictures were drawn but none could be written into the project.")
			: IngestError;

		OutError = State.Error;
		SetStatus(EMeshDefStatus::Failed, State.Error);
		return false;
	}

	// The newest becomes the main image. Where several came back, the first is the one the model
	// considers the front.
	MainImage = First;

	// **A declared view set claims the extra slots; anything else does not.** Several pictures from a
	// drawing model are usually several attempts, and promoting those would hand a reconstruction
	// contradictory evidence nobody asked it to fuse. A pipeline that produced a genuine orbit says
	// so, and then leaving the views sitting unassigned in the gallery is the wrong kind of caution -
	// it means paying for four views and reconstructing from one.
	if (Drawn.bIsViewSet && Views.Num() > 0)
	{
		const int32 Allowed = MaxExtraViews();

		if (Allowed <= 0)
		{
			UE_LOG(LogMeshForge, Log,
				TEXT("%s: drew %d extra view(s), and this definition's generator reconstructs from "
					 "one picture - they are in the gallery but will not be sent."),
				*GetName(), Views.Num());
		}
		else
		{
			ExtraViews.Reset();

			for (const TSoftObjectPtr<UTexture2D>& View : Views)
			{
				if (ExtraViews.Num() >= Allowed) { break; }
				ExtraViews.Add(View);
			}

			UE_LOG(LogMeshForge, Log, TEXT("%s: claimed %d extra view slot(s) from the view set."),
				*GetName(), ExtraViews.Num());
		}
	}

	State.Status     = EMeshStageStatus::Ready;
	State.LastRunUtc = FDateTime::UtcNow();
	State.InputsHash = ComputeStageHash(EMeshStage::Concept);

	// The stages after this were made from a different picture and must say so. Their outputs stay.
	RefreshStaleness();

	SetStatus(EMeshDefStatus::Draft, FString());

	UE_LOG(LogMeshForge, Log, TEXT("%s: ingested %d concept image(s)."), *GetName(), Drawn.Images.Num());
	return true;
}


int32 UMeshDef::MaxExtraViews() const
{
	// **This answers what the *model* can read, and deliberately not whether multi-view is switched
	// on.** Folding the switch in here made one number mean two different things, and the panel then
	// blamed the model for a choice the user had made: with the toggle off it said "this generator
	// reconstructs from one picture", which is a statement about the generator and was not true.
	// Whether the views are actually sent is UsableExtraViews' question.

	// **The pipeline first, because it is what knows the model.** A provider-wide answer is a guess
	// the moment one vendor's models differ from each other, and the cost of that guess is paid by
	// somebody who drew four views, was told four would be used, and got a reconstruction from one.
	if (MeshPipeline != nullptr)
	{
		const int32 Declared = MeshPipeline->GetMaxExtraViews();

		if (Declared >= 0)
		{
			return Declared;
		}
	}

	// Otherwise the provider, which is right for a vendor whose models all behave the same. Three
	// because that is what is left of a four-image endpoint once the main picture has its slot.
	const FName Named = MeshPipeline ? MeshPipeline->GetProviderId() : ProviderId;

	if (Named.IsNone())
	{
		return 0;
	}

	if (FMeshForgeModule* Module = FMeshForgeModule::GetPtr())
	{
		if (const TSharedPtr<IMeshProvider> Provider = Module->FindProvider(Named))
		{
			return Provider->GetCaps().bSupportsMultipleImages ? 3 : 0;
		}
	}

	return 0;
}

TSoftObjectPtr<UTexture2D> UMeshDef::ResolveMainImage() const
{
	if (!MainImage.IsNull())
	{
		return MainImage;
	}

	if (!SourceImage.IsNull())
	{
		return SourceImage;
	}

	if (ConceptImages.IsValidIndex(SelectedConcept))
	{
		return ConceptImages[SelectedConcept];
	}

	// The newest, which is what somebody who has just pressed Draw expects to be looking at.
	return ConceptImages.Num() > 0 ? ConceptImages.Last() : TSoftObjectPtr<UTexture2D>();
}

TArray<TSoftObjectPtr<UTexture2D>> UMeshDef::GatherImagePool() const
{
	TArray<TSoftObjectPtr<UTexture2D>> Pool;

	// Added first, because a picture somebody brought in deliberately is more likely to be the one
	// they want than the fifteenth thing a model drew.
	if (!SourceImage.IsNull())
	{
		Pool.Add(SourceImage);
	}

	for (const TSoftObjectPtr<UTexture2D>& Image : AddedImages)
	{
		if (!Image.IsNull()) { Pool.AddUnique(Image); }
	}

	for (const TSoftObjectPtr<UTexture2D>& Image : ConceptImages)
	{
		if (!Image.IsNull()) { Pool.AddUnique(Image); }
	}

	for (const TSoftObjectPtr<UTexture2D>& Image : ReferenceImages)
	{
		if (!Image.IsNull()) { Pool.AddUnique(Image); }
	}

	return Pool;
}


void UMeshDef::RefinishNow()
{
	UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get();

	if (Subsystem == nullptr)
	{
		MeshDefButtons::Toast(LOCTEXT("NoSubsystem", "MeshForge is not available."), false);
		return;
	}

	const FMeshImportOutcome Outcome = Subsystem->RefinishMesh(this);

	// Recorded either way, so the Import row stops claiming it has never run beside a mesh that is
	// plainly finished - and so a failure says what went wrong where somebody will look for it.
	FMeshStageState& State = Stages.FindOrAdd(EMeshStage::Import);
	State.LastRunUtc = FDateTime::UtcNow();

	if (!Outcome.bSuccess)
	{
		State.Status = EMeshStageStatus::Failed;
		State.Error  = Outcome.Error;

		MeshDefButtons::Toast(FText::FromString(Outcome.Error), false);
		return;
	}

	State.Status     = EMeshStageStatus::Ready;
	State.InputsHash = ComputeStageHash(EMeshStage::Import);
	State.Error.Reset();

	MeshDefButtons::Toast(FText::Format(
		LOCTEXT("Refinished", "{0}: {1} triangles, {2} collision primitive(s), {3}cm across."),
		FText::FromString(GetName()),
		FText::AsNumber(Outcome.TriangleCount),
		FText::AsNumber(Outcome.CollisionPrimitives),
		FText::AsNumber(FMath::RoundToInt(Outcome.BoundsSize.GetMax()))), true);
}

void UMeshDef::RunPostNow()
{
	UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get();

	if (Subsystem == nullptr)
	{
		MeshDefButtons::Toast(LOCTEXT("NoSubsystem", "MeshForge is not available."), false);
		return;
	}

	// Started, not run - the same shape as drawing and generating. Everything that can be decided
	// before any money is spent is decided now and reported now; everything after lands on the
	// stage and in the job list.
	FString Error;

	if (!Subsystem->StartPostProcessing(this, Error).IsValid())
	{
		MeshDefButtons::Toast(FText::FromString(Error), false);
		return;
	}

	MeshDefButtons::Toast(FText::Format(
		LOCTEXT("PostStarted", "{0}: post-processing. The mesh is re-imported when it finishes."),
		FText::FromString(GetName())), true);
}

void UMeshDef::ExportGlbNow()
{
	// The supplied mesh first. Where a definition has one, that is the mesh it is *about*, and
	// exporting the generated result instead would quietly hand somebody the wrong file.
	UStaticMesh* Mesh = SourceMesh.IsNull()
		? ImportedMesh.LoadSynchronous()
		: SourceMesh.LoadSynchronous();

	if (Mesh == nullptr)
	{
		MeshDefButtons::Toast(LOCTEXT("NothingToExport",
			"Nothing to export. Set a source mesh, or generate and import one first."), false);
		return;
	}

	const FString Path = UMeshForgeSettings::Get()->GetStagingDirectory()
		/ FString::Printf(TEXT("%s.glb"), *Mesh->GetName());

	const FMeshExportResult Result = FMeshExporter::ToGlb(Mesh, Path);

	if (!Result.bSuccess)
	{
		MeshDefButtons::Toast(FText::FromString(Result.Error), false);
		return;
	}

	for (const FString& Warning : Result.Warnings)
	{
		UE_LOG(LogMeshForge, Warning, TEXT("'%s': %s"), *Mesh->GetName(), *Warning);
	}

	// The path is in the message rather than only in the log, because the whole value of this button
	// is being able to go and look at the file.
	MeshDefButtons::Toast(FText::Format(
		LOCTEXT("Exported", "{0}: {1} KB written to {2}"),
		FText::FromString(Mesh->GetName()),
		FText::AsNumber(static_cast<int32>(Result.SizeBytes / 1024)),
		FText::FromString(Result.Path)), true);
}

void UMeshDef::EstimateNow()
{
	UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get();

	if (Subsystem == nullptr)
	{
		MeshDefButtons::Toast(LOCTEXT("NoSubsystem", "MeshForge is not available."), false);
		return;
	}

	TSharedPtr<IMeshProvider> Provider = Subsystem->ResolveProvider(this);

	if (!Provider.IsValid())
	{
		MeshDefButtons::Toast(LOCTEXT("NoProvider",
			"No mesh provider. Enable a provider plugin and choose one in Project Settings."), false);
		return;
	}

	FString Reason;
	const bool bReady = Provider->IsAvailable(Reason);
	const FMeshProviderCaps Caps = Provider->GetCaps();
	const int32 Takes = Caps.bSupportsVariants ? FMath::Max(1, Variants) : 1;

	// Priced through the same resolver the submission uses, so this cannot quote one product and
	// generate another.
	FString Model;
	const FMeshControl Priced = Subsystem->ResolveControl(this, Model);
	const FString PerTake = Provider->DescribeCost(
		Priced, Subsystem->HasReferenceImage(this), Subsystem->UsableExtraViews(this));

	if (!bReady)
	{
		MeshDefButtons::Toast(FText::FromString(Reason), false);
		return;
	}

	// The price where the provider can give one, and never an invented one: on most vendors what a
	// generation costs is a property of somebody's billing plan rather than of the API, and an
	// estimate is the last thing anybody reads before spending.
	const FText Money = PerTake.IsEmpty()
		? (Caps.bIsMetered
			? LOCTEXT("Metered", "Every take is billed, kept or discarded.")
			: LOCTEXT("Free", "Runs locally, so takes are free."))
		: FText::Format(LOCTEXT("MeteredAt", "{0} per take, billed whether it is kept or not."),
			FText::FromString(PerTake));

	MeshDefButtons::Toast(FText::Format(
		LOCTEXT("Estimate", "{0}: {1} take(s), roughly {2}s. {3}"),
		FText::FromString(Provider->GetDisplayName()),
		FText::AsNumber(Takes),
		FText::AsNumber(Takes * FMath::Max(1, Caps.TypicalSecondsPerMesh)),
		Money), true);
}

#undef LOCTEXT_NAMESPACE

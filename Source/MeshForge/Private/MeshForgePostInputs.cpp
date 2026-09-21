#include "MeshForgeSubsystem.h"
#include "MeshDef.h"
#include "MeshForgeSettings.h"
#include "ForgeLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/Paths.h"

TArray<FMeshPostOutput> UMeshForgeSubsystem::GetPostInputChoices(const UMeshDef* Def) const
{
	if (!Def) return {};
	TArray<FMeshPostOutput> Choices = Def->PostOutputs;
	TSet<FSoftObjectPath> Seen;
	for (const auto& Output : Choices) Seen.Add(Output.Asset.ToSoftObjectPath());
	TMap<FString, FForgeTakeRecord> Records;
	for (const auto& Record : FForgeLibrary::ListTakes(TEXT("MeshForge"), Def->GetName())) Records.Add(Record.TakeId, Record);
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*(UMeshForgeSettings::Get()->GetGeneratedFolder(Def->GetName()) / TEXT("Post"))));
	Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Assets);
	for (const FAssetData& Asset : Assets)
	{
		if (Seen.Contains(Asset.GetSoftObjectPath())) continue;
		FMeshPostOutput Output;
		Output.Asset = TSoftObjectPtr<UObject>(Asset.GetSoftObjectPath());
		Output.Step = TEXT("Saved post output");
		TArray<FString> Parts;
		Asset.PackageName.ToString().ParseIntoArray(Parts, TEXT("/"));
		const int32 Post = Parts.IndexOfByKey(TEXT("Post"));
		if (Parts.IsValidIndex(Post + 1)) Output.TakeId = Parts[Post + 1];
		if (const FForgeTakeRecord* Record = Records.Find(Output.TakeId))
		{
			Output.CreatedUtc = Record->FinishedUtc;
			FString ClassName = Record->Settings.FindRef(TEXT("pipelineClass"));
			// Earlier versions recorded the whole chain; only its final step produced this take.
			int32 LastIndex = -1;
			for (const auto& Pair : Record->Settings)
			{
				const FString Suffix = Pair.Key.Mid(4);
				if (Pair.Key.StartsWith(TEXT("step")) && Suffix.IsNumeric() && FCString::Atoi(*Suffix) > LastIndex)
				{
					LastIndex = FCString::Atoi(*Suffix);
					const int32 Space = Pair.Value.Find(TEXT(" "));
					ClassName = Space == INDEX_NONE ? Pair.Value : Pair.Value.Left(Space);
				}
			}
			if (!ClassName.IsEmpty()) Output.Step = ClassName;
		}
		Choices.Add(Output);
	}
	Choices.Sort([](const auto& A, const auto& B) { return A.CreatedUtc > B.CreatedUtc; });
	return Choices;
}

TArray<FMeshPostOutput> UMeshForgeSubsystem::GetStepOutputs(const UMeshDef* Def, const FGuid& StepId) const
{
	TArray<FMeshPostOutput> Outputs;
	if (!Def || !StepId.IsValid()) return Outputs;
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	// PostOutputs is appended as runs finish, so walking it backwards is newest first. A run whose
	// asset was deleted is left out: offering it would be offering an input that fails at Run.
	for (int32 Index = Def->PostOutputs.Num() - 1; Index >= 0; --Index)
	{
		const FMeshPostOutput& Output = Def->PostOutputs[Index];
		if (Output.StepId == StepId && !Output.Asset.IsNull()
			&& Registry.GetAssetByObjectPath(Output.Asset.ToSoftObjectPath()).IsValid())
		{
			Outputs.Add(Output);
		}
	}
	return Outputs;
}

bool UMeshForgeSubsystem::FindStepOutput(const UMeshDef* Def, const FGuid& StepId, const FString& TakeId,
	FMeshPostOutput& OutOutput) const
{
	for (const FMeshPostOutput& Output : GetStepOutputs(Def, StepId))
	{
		if (TakeId.IsEmpty() || Output.TakeId == TakeId)
		{
			OutOutput = Output;
			return true;
		}
	}
	return false;
}

bool UMeshForgeSubsystem::ResolvePostStepMesh(const UMeshDef* Def, int32 Index,
	TSoftObjectPtr<UObject>& Mesh, FString& Error) const
{
	Mesh.Reset(); Error.Reset();
	if (!Def || !Def->PostPipelines.IsValidIndex(Index) || !Def->PostPipelines[Index])
	{ Error = TEXT("This step no longer exists."); return false; }
	const UMeshPostPipeline* Step = Def->PostPipelines[Index];
	const bool bNative = Step->IsNativeStep();
	const bool bFromMeshStage = Step->StartsFromMeshStage();
	if (Step->InputSource == EMeshPostInputSource::SelectedMesh) Mesh = Step->InputMesh;
	else if (bFromMeshStage && !Step->InputTakeId.IsEmpty() && Def->SourceMesh.IsNull())
	{
		// One particular take. Null with no error, like the selected take below: the chain reads its file.
		const FMeshCandidate* Take = Def->FindCandidate(Step->InputTakeId);
		if (Take == nullptr || !Take->IsUsable())
		{ Error = TEXT("The chosen take of the Mesh stage no longer exists. Choose another take."); return false; }
		if (bNative)
		{
			Error = FString::Printf(TEXT("'%s' works on an asset, and a take is a file until it is imported. Import it, "
				"then choose it under Selected mesh / saved output."), *Step->GetStepDisplayName().ToString());
			return false;
		}
		if (!FPaths::FileExists(Take->LocalArtifactPath))
		{ Error = TEXT("The chosen take has no file on this machine. Import it once - that downloads it - then run."); return false; }
		return true;
	}
	else if (Step->InputSource == EMeshPostInputSource::StepOutput && !bFromMeshStage)
	{
		const TObjectPtr<UMeshPostPipeline>* Source = Step->InputStepId.IsValid()
			? Def->PostPipelines.FindByPredicate([Step](const UMeshPostPipeline* Other) { return Other && Other->StepId == Step->InputStepId; })
			: nullptr;
		if (!Source) { Error = TEXT("Choose which step's output this step starts from."); return false; }
		if (*Source == Step) { Error = TEXT("A step cannot start from its own output. Choose another step."); return false; }
		const FString SourceName = (*Source)->GetStepDisplayName().ToString();
		FMeshPostOutput Output;
		if (!FindStepOutput(Def, Step->InputStepId, Step->InputTakeId, Output))
		{
			Error = Step->InputTakeId.IsEmpty()
				? FString::Printf(TEXT("'%s' has no output yet. Run it first, or choose another step."), *SourceName)
				: FString::Printf(TEXT("The chosen run of '%s' no longer exists. Choose another run."), *SourceName);
			return false;
		}
		Mesh = Output.Asset;
	}
	// The Mesh stage's selected take - or its Source Mesh - is exactly what "Original mesh" means.
	else if (Step->InputSource == EMeshPostInputSource::OriginalMesh || bFromMeshStage || Index == 0)
	{
		Mesh = TSoftObjectPtr<UObject>(Def->SourceMesh.ToSoftObjectPath());
		if (Mesh.IsNull() && bNative)
		{
			// A native step cannot read a take's bytes, but one that wants a skeletal mesh can read the
			// definition's latest skeletal result. Never the imported static mesh: for the skinning step
			// that is usually the unwrapped take, and skinning that would succeed and be wrong.
			if (!Def->ImportedSkeletalMesh.IsNull() && Step->AcceptsNativeInput(USkeletalMesh::StaticClass()))
				Mesh = TSoftObjectPtr<UObject>(Def->ImportedSkeletalMesh.ToSoftObjectPath());
		}
		if (Mesh.IsNull())
		{
			const FMeshCandidate* Candidate = Def->FindSelectedCandidate();
			if (Candidate && Candidate->IsUsable() && FPaths::FileExists(Candidate->LocalArtifactPath) && !bNative) return true;
			Error = TEXT("No valid original mesh. Choose a Source Mesh, or select a mesh / saved output as this step's input.");
			return false;
		}
	}
	else
	{
		const UMeshPostPipeline* Previous = Def->PostPipelines[Index - 1];
		if (!Previous) { Error = TEXT("The previous step is missing."); return false; }
		int32 MatchingClasses = 0;
		for (const UMeshPostPipeline* Other : Def->PostPipelines)
			if (Other && Other->GetClass() == Previous->GetClass()) ++MatchingClasses;
		for (const FMeshPostOutput& Output : GetPostInputChoices(Def))
		{
			const bool Exact = Previous->StepId.IsValid() && Output.StepId == Previous->StepId;
			const bool Legacy = !Output.StepId.IsValid() && MatchingClasses == 1 &&
				(Output.Step == Previous->GetClass()->GetName() || Output.Step == Previous->GetClass()->GetDisplayNameText().ToString());
			if (Exact || Legacy) { Mesh = Output.Asset; break; }
		}
		if (Mesh.IsNull())
		{
			Error = TEXT("No valid input: the previous step has no saved output. Run it first, or select a mesh / saved output.");
			return false;
		}
	}
	if (Mesh.IsNull()) { Error = TEXT("Select an input mesh or saved output."); return false; }
	const FAssetData Data = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
		.Get().GetAssetByObjectPath(Mesh.ToSoftObjectPath());
	if (!Data.IsValid())
	{ Error = TEXT("The selected input is missing."); return false; }
	if (bNative)
	{
		const UClass* Class = Data.GetClass(EResolveClass::Yes);
		if (!Step->AcceptsNativeInput(Class))
		{
			Error = FString::Printf(TEXT("'%s' cannot work on %s. Select a different input."),
				*Step->GetStepDisplayName().ToString(), Class ? *Class->GetName() : *Data.AssetClassPath.ToString());
			return false;
		}
		return true;
	}
	if (Data.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName())
	{ Error = TEXT("The selected input is missing or is not a static mesh."); return false; }
	return true;
}

FString UMeshForgeSubsystem::PostStepBlockedReason(const UMeshDef* Def, int32 Index) const
{
	if (!Def || !Def->PostPipelines.IsValidIndex(Index) || !Def->PostPipelines[Index]) return TEXT("This step no longer exists.");
	if (!Def->StageSwitches.bPost) return Def->DescribeDisabledStage(EMeshStage::Post);
	if (Def->IsBusy() || IsStageRunning(Def, EMeshStage::Post)) return TEXT("Wait for the current job to finish.");
	if (!Def->PostPipelines[Index]->bEnabled) return TEXT("Enable this step to run it.");
	TSoftObjectPtr<UObject> Mesh;
	FString Error;
	if (!ResolvePostStepMesh(Def, Index, Mesh, Error)) return Error;
	return Def->PostPipelines[Index]->Validate();
}

FGuid UMeshForgeSubsystem::RunPostStep(UMeshDef* Definition, int32 StepIndex, FString& Error)
{
	Error = PostStepBlockedReason(Definition, StepIndex);
	return Error.IsEmpty() ? StartPostProcessing(Definition, Error, StepIndex) : FGuid();
}

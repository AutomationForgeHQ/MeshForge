#include "MeshForgeSubsystem.h"
#include "MeshDef.h"
#include "MeshForgeSettings.h"
#include "ForgeLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
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

bool UMeshForgeSubsystem::ResolvePostStepMesh(const UMeshDef* Def, int32 Index,
	TSoftObjectPtr<UStaticMesh>& Mesh, FString& Error) const
{
	Mesh.Reset(); Error.Reset();
	if (!Def || !Def->PostPipelines.IsValidIndex(Index) || !Def->PostPipelines[Index])
	{ Error = TEXT("This step no longer exists."); return false; }
	const UMeshPostPipeline* Step = Def->PostPipelines[Index];
	if (Step->InputSource == EMeshPostInputSource::SelectedMesh) Mesh = Step->InputMesh;
	else if (Step->InputSource == EMeshPostInputSource::OriginalMesh || Index == 0)
	{
		Mesh = Def->SourceMesh;
		if (Mesh.IsNull())
		{
			const FMeshCandidate* Candidate = Def->FindSelectedCandidate();
			if (Candidate && Candidate->IsUsable() && FPaths::FileExists(Candidate->LocalArtifactPath) && !Step->ProducesSkeletalMesh()) return true;
			Error = TEXT("No valid original mesh. Choose a Source Mesh (or an imported mesh for skeletal creation).");
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
			if (Exact || Legacy) { Mesh = TSoftObjectPtr<UStaticMesh>(Output.Asset.ToSoftObjectPath()); break; }
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
	if (!Data.IsValid() || Data.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName())
	{ Error = TEXT("The selected input is missing or is not a static mesh."); return false; }
	return true;
}

FString UMeshForgeSubsystem::PostStepBlockedReason(const UMeshDef* Def, int32 Index) const
{
	if (!Def || !Def->PostPipelines.IsValidIndex(Index) || !Def->PostPipelines[Index]) return TEXT("This step no longer exists.");
	if (Def->IsBusy() || IsStageRunning(Def, EMeshStage::Post)) return TEXT("Wait for the current job to finish.");
	if (!Def->PostPipelines[Index]->bEnabled) return TEXT("Enable this step to run it.");
	TSoftObjectPtr<UStaticMesh> Mesh;
	FString Error;
	if (!ResolvePostStepMesh(Def, Index, Mesh, Error)) return Error;
	return Def->PostPipelines[Index]->Validate();
}

FGuid UMeshForgeSubsystem::RunPostStep(UMeshDef* Definition, int32 StepIndex, FString& Error)
{
	Error = PostStepBlockedReason(Definition, StepIndex);
	return Error.IsEmpty() ? StartPostProcessing(Definition, Error, StepIndex) : FGuid();
}

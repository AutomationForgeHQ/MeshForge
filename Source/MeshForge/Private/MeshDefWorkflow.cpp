// Applying a Mesh Workflow to a definition, and saying what has changed since.

#include "MeshDef.h"

#include "MeshForge.h"
#include "MeshRegisteredPostStep.h"
#include "MeshWorkflow.h"
#include "UObject/UObjectGlobals.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace MeshDefWorkflow
{
	/**
	 * A copy of a pipeline owned by the definition.
	 *
	 * A preset is a standalone asset; its copy must not be - two public, standalone objects in one package
	 * is two assets in a file that should hold one. Named uniquely so it never lands on the pipeline it is
	 * replacing, which is still in the package until the definition is saved.
	 */
	template <typename PipelineType>
	static PipelineType* CopyInto(UObject* Outer, const PipelineType* Source)
	{
		if (Source == nullptr)
		{
			return nullptr;
		}

		FObjectDuplicationParameters Params = InitStaticDuplicateObjectParams(Source, Outer,
			MakeUniqueObjectName(Outer, Source->GetClass(), Source->GetFName()));
		Params.FlagMask &= ~(RF_Public | RF_Standalone | RF_ArchetypeObject);
		Params.ApplyFlags |= RF_Transactional;
		PipelineType* Copy = Cast<PipelineType>(StaticDuplicateObjectEx(Params));

		// A workflow is how something is made, a prompt is what: saving one leaves the prompts behind, and
		// applying one keeps the definition's own (ApplyWorkflow carries them across).
		if (Copy != nullptr)
		{
			Copy->Prompt.Reset();
		}
		return Copy;
	}

	/** Whether two post steps are the same kind of step, so the new one may inherit the old one's history. */
	static bool SameKind(const UMeshPostPipeline* A, const UMeshPostPipeline* B)
	{
		if (A == nullptr || B == nullptr || A->GetClass() != B->GetClass())
		{
			return false;
		}
		const UMeshRegisteredPostStep* RA = Cast<UMeshRegisteredPostStep>(A);
		const UMeshRegisteredPostStep* RB = Cast<UMeshRegisteredPostStep>(B);
		return RA == nullptr || RA->StepType == RB->StepType;
	}

	/** What a pipeline would do, for comparing a definition's with its workflow's. */
	static FString SignatureOf(const UMeshForgePipeline* Pipeline)
	{
		return Pipeline == nullptr ? FString(TEXT("none")) : Pipeline->Signature() + (Pipeline->bEnabled ? TEXT("") : TEXT("|off"));
	}

	/**
	 * A post step's signature for comparison, with the step it starts from named by position, not id.
	 *
	 * The id differs between a definition and its workflow as soon as reapplying keeps a step's history,
	 * and comparing ids would report every such step as changed. Its position in the chain does not.
	 */
	template <typename ChainType>
	static FString PostSignatureOf(const UMeshPostPipeline* Step, const ChainType& Chain,
		TFunctionRef<const UMeshPostPipeline*(const typename ChainType::ElementType&)> Get)
	{
		FString Signature = SignatureOf(Step);
		if (Step == nullptr || !Step->InputStepId.IsValid())
		{
			return Signature;
		}

		int32 Referenced = INDEX_NONE;
		for (int32 Index = 0; Index < Chain.Num(); ++Index)
		{
			const UMeshPostPipeline* Other = Get(Chain[Index]);
			if (Other != nullptr && Other->StepId == Step->InputStepId)
			{
				Referenced = Index;
				break;
			}
		}

		if (const FProperty* Property = UMeshPostPipeline::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UMeshPostPipeline, InputStepId)))
		{
			FString Value;
			Property->ExportTextItem_Direct(Value, &Step->InputStepId, nullptr, nullptr, PPF_None);
			Signature.ReplaceInline(*FString::Printf(TEXT("InputStepId=%s;"), *Value),
				*FString::Printf(TEXT("InputStep=#%d;"), Referenced));
		}
		return Signature;
	}

	static FString Named(const UMeshForgePipeline* Pipeline)
	{
		if (const UMeshPostPipeline* Post = Cast<UMeshPostPipeline>(Pipeline))
		{
			return Post->GetStepDisplayName().ToString();
		}
		return Pipeline ? Pipeline->GetClass()->GetDisplayNameText().ToString() : FString(TEXT("nothing"));
	}
}

bool UMeshDef::ApplyWorkflow(const UMeshWorkflow* InWorkflow, TArray<FString>& OutNotes, FString& OutError)
{
	using namespace MeshDefWorkflow;

	OutNotes.Reset();
	OutError.Reset();

	if (InWorkflow == nullptr)
	{
		OutError = TEXT("No workflow to apply.");
		return false;
	}

	if (IsBusy())
	{
		OutError = TEXT("Wait for the current job to finish before changing this definition's workflow.");
		return false;
	}

	OutNotes = InWorkflow->DescribeMissingPresets();

	Modify();

	StageSwitches = InWorkflow->Stages;

	const TObjectPtr<UMeshImagePipeline> PreviousConcept = ConceptPipeline;
	const TObjectPtr<UMeshForgePipeline> PreviousMesh = MeshPipeline;
	const TArray<TObjectPtr<UMeshForgePipeline>> PreviousRefinement = RefinementPipelines;

	ConceptPipeline = CopyInto(this, UMeshWorkflow::Resolve(InWorkflow->Concept.Pipeline, InWorkflow->Concept.Preset));
	MeshPipeline    = CopyInto(this, UMeshWorkflow::Resolve(InWorkflow->Mesh.Pipeline, InWorkflow->Mesh.Preset));
	CarryPrompt(PreviousConcept, ConceptPipeline, Prompt);
	CarryPrompt(PreviousMesh, MeshPipeline, Prompt);

	if (MeshPipeline != nullptr && MeshPipeline->GetKind() != EMeshPipelineKind::Mesh)
	{
		OutNotes.Add(FString::Printf(TEXT("The workflow's Mesh slot holds '%s', which is not a mesh pipeline; it was left out."),
			*MeshPipeline->GetClass()->GetDisplayNameText().ToString()));
		MeshPipeline = nullptr;
	}

	RefinementPipelines.Reset();
	for (const FMeshWorkflowPipelineSlot& Slot : InWorkflow->References)
	{
		if (UMeshForgePipeline* Copy = CopyInto(this, UMeshWorkflow::Resolve(Slot.Pipeline, Slot.Preset)))
		{
			const int32 At = RefinementPipelines.Num();
			CarryPrompt(PreviousRefinement.IsValidIndex(At) ? PreviousRefinement[At].Get() : nullptr, Copy, Prompt);
			RefinementPipelines.Add(Copy);
		}
	}

	TArray<TObjectPtr<UMeshPostPipeline>> Previous = PostPipelines;
	PostPipelines.Reset();
	TArray<FGuid> FromWorkflow;   // each copy's id as the workflow had it, by position
	for (const FMeshWorkflowPostSlot& Slot : InWorkflow->Post)
	{
		if (UMeshPostPipeline* Copy = CopyInto(this, UMeshWorkflow::Resolve(Slot.Pipeline, Slot.Preset)))
		{
			FromWorkflow.Add(Copy->StepId);
			Copy->ForgetDefinitionState();
			PostPipelines.Add(Copy);
		}
	}

	// Reapplying keeps history: the step at a position, if it is the same kind, hands on its id so the
	// outputs it made stay linked. Those are settled first, because a step that is not kept takes the
	// workflow's own id - and after the chain was edited by hand that id can be one a kept step already
	// has, which would give two steps one history. Such a step gets a new id instead.
	TSet<FGuid> Taken;
	for (int32 Position = 0; Position < PostPipelines.Num(); ++Position)
	{
		const UMeshPostPipeline* Before = Previous.IsValidIndex(Position) ? Previous[Position].Get() : nullptr;
		if (SameKind(Before, PostPipelines[Position]))
		{
			PostPipelines[Position]->StepId = Before->StepId;
			PostPipelines[Position]->KeepDefinitionState(*Before);
			if (Before->StepId.IsValid())
			{
				Taken.Add(Before->StepId);
			}
		}
		else
		{
			PostPipelines[Position]->StepId.Invalidate();
		}
	}

	TMap<FGuid, FGuid> IdsFromWorkflow;   // the workflow's step id -> the id the step ends up with here
	for (int32 Position = 0; Position < PostPipelines.Num(); ++Position)
	{
		UMeshPostPipeline* Step = PostPipelines[Position];
		if (!Step->StepId.IsValid() && FromWorkflow[Position].IsValid())
		{
			Step->StepId = Taken.Contains(FromWorkflow[Position]) ? FGuid::NewGuid() : FromWorkflow[Position];
		}
		if (Step->StepId.IsValid())
		{
			Taken.Add(Step->StepId);
		}
		if (FromWorkflow[Position].IsValid())
		{
			IdsFromWorkflow.Add(FromWorkflow[Position], Step->StepId);
		}
	}

	// A step that starts from another step's output names it by id, and the id may just have changed.
	for (UMeshPostPipeline* Step : PostPipelines)
	{
		if (const FGuid* Remapped = IdsFromWorkflow.Find(Step->InputStepId))
		{
			Step->InputStepId = *Remapped;
		}
	}

	if (InWorkflow->bOverrideFinish)
	{
		Finish = InWorkflow->Finish;
	}

	Workflow = const_cast<UMeshWorkflow*>(InWorkflow);

	RefreshStaleness();
	MarkPackageDirty();

	UE_LOG(LogMeshForge, Log, TEXT("'%s': applied workflow '%s'%s"), *GetName(), *InWorkflow->GetName(),
		OutNotes.Num() > 0 ? *FString::Printf(TEXT(", with %d slot(s) left empty."), OutNotes.Num()) : TEXT("."));

	return true;
}

TArray<FString> UMeshDef::DescribeWorkflowChanges() const
{
	using namespace MeshDefWorkflow;

	TArray<FString> Changes;

	if (Workflow.IsNull())
	{
		return Changes;
	}

	const UMeshWorkflow* Source = Workflow.LoadSynchronous();
	if (Source == nullptr)
	{
		Changes.Add(TEXT("The workflow it was set up from no longer exists."));
		return Changes;
	}

	for (EMeshStage Stage : { EMeshStage::Concept, EMeshStage::References, EMeshStage::Mesh, EMeshStage::Post, EMeshStage::Import })
	{
		if (StageSwitches.IsOn(Stage) != Source->Stages.IsOn(Stage))
		{
			Changes.Add(FString::Printf(TEXT("%s: %s here, %s in the workflow"),
				*StaticEnum<EMeshStage>()->GetDisplayNameTextByValue(static_cast<int64>(Stage)).ToString(),
				StageSwitches.IsOn(Stage) ? TEXT("on") : TEXT("off"),
				Source->Stages.IsOn(Stage) ? TEXT("on") : TEXT("off")));
		}
	}

	const UMeshForgePipeline* WorkflowConcept = UMeshWorkflow::Resolve(Source->Concept.Pipeline, Source->Concept.Preset);
	if (SignatureOf(ConceptPipeline) != SignatureOf(WorkflowConcept))
	{
		Changes.Add(FString::Printf(TEXT("Concept: %s here, %s in the workflow"), *Named(ConceptPipeline), *Named(WorkflowConcept)));
	}

	const UMeshForgePipeline* WorkflowMesh = UMeshWorkflow::Resolve(Source->Mesh.Pipeline, Source->Mesh.Preset);
	if (SignatureOf(MeshPipeline) != SignatureOf(WorkflowMesh))
	{
		Changes.Add(FString::Printf(TEXT("Mesh: %s here, %s in the workflow"), *Named(MeshPipeline), *Named(WorkflowMesh)));
	}

	if (RefinementPipelines.Num() != Source->References.Num())
	{
		Changes.Add(FString::Printf(TEXT("References: %d step(s) here, %d in the workflow"), RefinementPipelines.Num(), Source->References.Num()));
	}
	else
	{
		for (int32 Index = 0; Index < RefinementPipelines.Num(); ++Index)
		{
			const FMeshWorkflowPipelineSlot& Slot = Source->References[Index];
			if (SignatureOf(RefinementPipelines[Index]) != SignatureOf(UMeshWorkflow::Resolve(Slot.Pipeline, Slot.Preset)))
			{
				Changes.Add(FString::Printf(TEXT("Reference step %d (%s)"), Index + 1, *Named(RefinementPipelines[Index])));
			}
		}
	}

	if (PostPipelines.Num() != Source->Post.Num())
	{
		Changes.Add(FString::Printf(TEXT("Post: %d step(s) here, %d in the workflow"), PostPipelines.Num(), Source->Post.Num()));
	}
	else
	{
		for (int32 Index = 0; Index < PostPipelines.Num(); ++Index)
		{
			const FMeshWorkflowPostSlot& Slot = Source->Post[Index];
			const FString Mine = PostSignatureOf(PostPipelines[Index].Get(), PostPipelines,
				[](const TObjectPtr<UMeshPostPipeline>& P) -> const UMeshPostPipeline* { return P.Get(); });
			const FString Theirs = PostSignatureOf(UMeshWorkflow::Resolve(Slot.Pipeline, Slot.Preset), Source->Post,
				[](const FMeshWorkflowPostSlot& S) -> const UMeshPostPipeline* { return UMeshWorkflow::Resolve(S.Pipeline, S.Preset); });
			if (Mine != Theirs)
			{
				Changes.Add(FString::Printf(TEXT("Post step %d (%s)"), Index + 1, *Named(PostPipelines[Index])));
			}
		}
	}

	if (Source->bOverrideFinish)
	{
		FString Mine, Theirs;
		FMeshFinishSettings::StaticStruct()->ExportText(Mine, &Finish, nullptr, nullptr, PPF_None, nullptr);
		FMeshFinishSettings::StaticStruct()->ExportText(Theirs, &Source->Finish, nullptr, nullptr, PPF_None, nullptr);
		if (Mine != Theirs)
		{
			Changes.Add(TEXT("Import settings"));
		}
	}

	return Changes;
}

UMeshWorkflow* UMeshWorkflow::CreateFromDefinition(const UMeshDef* Def, const FString& AssetPath, FString& OutError)
{
	using namespace MeshDefWorkflow;

	OutError.Reset();

	if (Def == nullptr)
	{
		OutError = TEXT("No definition to save as a workflow.");
		return nullptr;
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	const FString AssetName = FPackageName::GetShortName(PackageName);

	if (!FPackageName::IsValidLongPackageName(PackageName) || AssetName.IsEmpty())
	{
		OutError = FString::Printf(TEXT("'%s' is not a place an asset can be saved."), *AssetPath);
		return nullptr;
	}

	if (FPackageName::DoesPackageExist(PackageName) || FindObject<UObject>(nullptr, *(PackageName + TEXT(".") + AssetName)))
	{
		OutError = FString::Printf(TEXT("An asset already exists at '%s'. Choose another name."), *PackageName);
		return nullptr;
	}

	UPackage* Package = CreatePackage(*PackageName);
	UMeshWorkflow* Workflow = NewObject<UMeshWorkflow>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);

	Workflow->Description = FText::FromString(FString::Printf(TEXT("Saved from %s."), *Def->GetName()));
	Workflow->Stages = Def->StageSwitches;
	Workflow->Concept.Pipeline = CopyInto(Workflow, Def->ConceptPipeline.Get());
	Workflow->Mesh.Pipeline = CopyInto(Workflow, Def->MeshPipeline.Get());

	for (const TObjectPtr<UMeshForgePipeline>& Pipeline : Def->RefinementPipelines)
	{
		FMeshWorkflowPipelineSlot& Slot = Workflow->References.AddDefaulted_GetRef();
		Slot.Pipeline = CopyInto(Workflow, Pipeline.Get());
	}

	for (const TObjectPtr<UMeshPostPipeline>& Pipeline : Def->PostPipelines)
	{
		FMeshWorkflowPostSlot& Slot = Workflow->Post.AddDefaulted_GetRef();
		Slot.Pipeline = CopyInto(Workflow, Pipeline.Get());
		if (Slot.Pipeline != nullptr)
		{
			// Step ids are kept, so a step that starts from another step's output still names it; the run
			// it picked and the session it last opened are this definition's and are not.
			Slot.Pipeline->InputTakeId.Reset();
			Slot.Pipeline->ForgetDefinitionState();
		}
	}

	Workflow->bOverrideFinish = true;
	Workflow->Finish = Def->Finish;

	FAssetRegistryModule::AssetCreated(Workflow);
	Workflow->MarkPackageDirty();

	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Workflow, *Filename, Args))
	{
		OutError = FString::Printf(TEXT("The workflow was made but could not be saved to '%s'."), *Filename);
	}

	UE_LOG(LogMeshForge, Log, TEXT("Saved '%s' as workflow %s."), *Def->GetName(), *Workflow->GetPathName());
	return Workflow;
}

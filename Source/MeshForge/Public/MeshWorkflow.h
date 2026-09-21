// A reusable setup for a Mesh Definition: which stages it uses, and the pipelines those stages run.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Misc/PackageName.h"
#include "MeshForgeTypes.h"
#include "MeshImagePipeline.h"
#include "MeshPostPipeline.h"
#include "MeshWorkflow.generated.h"

class UMeshDef;

/**
 * One pipeline a workflow puts on a stage: a configured copy held here, or a pipeline asset elsewhere.
 *
 * **The soft pointer is what lets a workflow name another plugin's pipeline without depending on it.**
 * A garment workflow shipped in MeshForgeGarment can point at a Meshy preset MeshForgeCloud ships; where
 * MeshForgeCloud is not installed the path simply does not resolve - no load warning, and the workflow
 * says which plugin that slot needs. A copy held inline is self-contained, and is what Save as Workflow
 * writes. When both are set, the copy wins.
 */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshWorkflowImageSlot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Instanced, BlueprintReadWrite, Category = "Slot")
	TObjectPtr<UMeshImagePipeline> Pipeline;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slot")
	TSoftObjectPtr<UMeshImagePipeline> Preset;
};

/** A slot for a mesh or refinement pipeline. See FMeshWorkflowImageSlot. */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshWorkflowPipelineSlot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Instanced, BlueprintReadWrite, Category = "Slot")
	TObjectPtr<UMeshForgePipeline> Pipeline;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slot")
	TSoftObjectPtr<UMeshForgePipeline> Preset;
};

/** A slot for one post-processing step. See FMeshWorkflowImageSlot. */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshWorkflowPostSlot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Instanced, BlueprintReadWrite, Category = "Slot")
	TObjectPtr<UMeshPostPipeline> Pipeline;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slot")
	TSoftObjectPtr<UMeshPostPipeline> Preset;
};

/**
 * A Mesh Workflow: which of a definition's five stages it uses, and what those stages run.
 *
 * **Applied as a template.** Choosing a workflow copies its switches, pipelines, post steps and, where it
 * says so, its finish settings into the definition; the definition then remembers which workflow it came
 * from and can say what it has changed since. Editing a workflow later changes no definition until
 * somebody reapplies it - one edit cannot turn forty meshes stale.
 *
 * Nothing about what a definition is *about* is ever copied: its prompt, pictures, Source Mesh, takes
 * and outputs are its own.
 *
 * Workflows ship in plugin Content folders and are ordinary editable assets. The easiest way to make
 * one is to set a definition up in its panel and use Save as Workflow.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Mesh Workflow"))
class MESHFORGE_API UMeshWorkflow : public UDataAsset
{
	GENERATED_BODY()

public:

	/** What the workflow is called in the picker. Empty uses the asset name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Workflow")
	FText DisplayName;

	/** What it is for, in a sentence or two. Shown as the picker's tooltip. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Workflow", meta = (MultiLine = true))
	FText Description;

	/** Which stages a definition using this workflow runs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Workflow")
	FMeshStageSwitches Stages;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "1 Concept")
	FMeshWorkflowImageSlot Concept;

	/** Refinement pipelines, in order. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "2 References")
	TArray<FMeshWorkflowPipelineSlot> References;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "3 Mesh")
	FMeshWorkflowPipelineSlot Mesh;

	/** Post-processing steps, in the order they run. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "4 Post")
	TArray<FMeshWorkflowPostSlot> Post;

	/**
	 * Copy Finish into the definition too. Off leaves a definition's import settings alone.
	 *
	 * On for a workflow whose result must import a particular way - a garment keeps the origin it was
	 * fitted at, where a prop's finish would re-centre it at the character's feet.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "5 Import")
	bool bOverrideFinish = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "5 Import", meta = (EditCondition = "bOverrideFinish"))
	FMeshFinishSettings Finish;

	/** The name to show: DisplayName, or the asset name. */
	FText GetShownName() const;

	/**
	 * Slots whose preset does not resolve, as sentences naming the plugin each needs. Empty when every
	 * slot resolves. Loads the presets it asks about.
	 */
	TArray<FString> DescribeMissingPresets() const;

	/**
	 * A new workflow asset made from a definition as it stands: its switches, a copy of every pipeline and
	 * post step, and its finish settings. Saved at AssetPath ("/Game/Workflows/WF_Name"). Null with a
	 * sentence when it could not be made; a workflow that was made but not saved returns with the sentence.
	 */
	static UMeshWorkflow* CreateFromDefinition(const UMeshDef* Def, const FString& AssetPath, FString& OutError);

	/** The plugin a content path belongs to - "/MeshForgeCloud/Pipelines/X" is MeshForgeCloud. */
	static FString PluginOfPath(const FSoftObjectPath& Path);

	/**
	 * A slot's pipeline: its copy, or its preset when that preset's package exists. Null otherwise.
	 *
	 * The package is checked before loading, because loading a path from a plugin that is not installed
	 * writes a warning to every customer's log - exactly what a soft pointer here is meant to avoid.
	 */
	template <typename PipelineType>
	static PipelineType* Resolve(const TObjectPtr<PipelineType>& Copy, const TSoftObjectPtr<PipelineType>& Preset)
	{
		if (Copy != nullptr)
		{
			return Copy.Get();
		}
		if (Preset.IsNull() || !FPackageName::DoesPackageExist(Preset.ToSoftObjectPath().GetLongPackageName()))
		{
			return nullptr;
		}
		return Preset.LoadSynchronous();
	}
};

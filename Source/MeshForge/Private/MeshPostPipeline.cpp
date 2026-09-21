#include "MeshPostPipeline.h"

#include "Engine/StaticMesh.h"

// "MESH" "STAG" "E" - readable in a hex dump of a saved definition, and not a value NewGuid can produce.
const FGuid UMeshPostPipeline::MeshStageSourceId(0x4D455348, 0x53544147, 0x45000000, 0x00000001);

FString UMeshPostPipeline::ChainOrderProblem(TConstArrayView<const UMeshPostPipeline*> Steps)
{
	// Once a chain reaches a step that works on assets, it cannot go back to steps that work on glTF:
	// nothing turns a skeletal mesh or an inventory item back into the bytes those read.
	const UMeshPostPipeline* LastAssetStep = nullptr;
	for (const UMeshPostPipeline* Step : Steps)
	{
		if (Step == nullptr || !Step->bEnabled)
		{
			continue;
		}
		if (LastAssetStep != nullptr && !Step->IsNativeStep())
		{
			return FString::Printf(TEXT("'%s' works on geometry and cannot come after '%s', which makes an asset. Move it earlier in the chain."),
				*Step->GetStepDisplayName().ToString(), *LastAssetStep->GetStepDisplayName().ToString());
		}
		if (Step->IsNativeStep())
		{
			LastAssetStep = Step;
		}
	}
	return FString();
}

bool UMeshPostPipeline::AcceptsNativeInput(const UClass* InputClass) const
{
	// The skeletal step's own contract, which is the only native step that existed before this one.
	return ProducesSkeletalMesh() && InputClass != nullptr && InputClass->IsChildOf(UStaticMesh::StaticClass());
}

UObject* UMeshPostPipeline::CreateNativeOutput(UObject* Input, FMeshPostNativeContext& Context, FString& Error) const
{
	if (UStaticMesh* Static = Cast<UStaticMesh>(Input))
	{
		return CreateSkeletalOutput(Static, Context.AssetPath, Error);
	}

	Error = FString::Printf(TEXT("'%s' works on a static mesh and was given %s."),
		*GetStepDisplayName().ToString(), Input ? *Input->GetClass()->GetName() : TEXT("nothing"));
	return nullptr;
}

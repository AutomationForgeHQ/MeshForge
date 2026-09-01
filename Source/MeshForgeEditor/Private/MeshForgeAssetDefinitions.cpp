// Copyright Blackcode SA. All rights reserved.

#include "MeshForgeAssetDefinitions.h"

#include "MeshDef.h"
#include "MeshDefEditorToolkit.h"

#include "MeshDef.h"

#define LOCTEXT_NAMESPACE "MeshForgeEditor"

namespace
{
	/** One category for the whole family, with a submenu per set, so the types sit together. */
	const TArray<FAssetCategoryPath>& ForgeCategories()
	{
		static const TArray<FAssetCategoryPath> Categories
		{
			FAssetCategoryPath(
				FAssetCategoryPath(LOCTEXT("AutomationForge", "Automation Forge")),
				LOCTEXT("MeshForge", "MeshForge"))
		};
		return Categories;
	}
}

FText UMeshDefAssetDefinition::GetAssetDisplayName() const
{
	return LOCTEXT("MeshDef", "Mesh Definition");
}

FLinearColor UMeshDefAssetDefinition::GetAssetColor() const
{
	// Warm, to sit apart from MotionForge's blue at a glance in a mixed folder.
	return FLinearColor(0.86f, 0.55f, 0.28f);
}

TSoftClassPtr<UObject> UMeshDefAssetDefinition::GetAssetClass() const
{
	return UMeshDef::StaticClass();
}

TConstArrayView<FAssetCategoryPath> UMeshDefAssetDefinition::GetAssetCategories() const
{
	return ForgeCategories();
}

EAssetCommandResult UMeshDefAssetDefinition::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	for (UMeshDef* Def : OpenArgs.LoadObjects<UMeshDef>())
	{
		const TSharedRef<FMeshDefEditorToolkit> Toolkit = MakeShared<FMeshDefEditorToolkit>();
		Toolkit->Initialise(OpenArgs.GetToolkitMode(), OpenArgs.ToolkitHost, Def);
	}

	return EAssetCommandResult::Handled;
}


#undef LOCTEXT_NAMESPACE

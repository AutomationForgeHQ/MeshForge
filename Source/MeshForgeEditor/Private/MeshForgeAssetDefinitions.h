// Copyright Blackcode SA. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetDefinitionDefault.h"

#include "MeshForgeAssetDefinitions.generated.h"

/**
 * Where a Mesh Definition lives in the Content Browser, and what it looks like.
 *
 * The category matters more than it looks: a project that installs the whole family gets a dozen
 * asset types, and dropping them into Miscellaneous alongside everything else is how somebody fails
 * to find the thing they made this morning.
 *
 * The default editor is kept deliberately. A mesh definition is a page of fields and a list of
 * takes; a details panel shows all of it, and a bespoke window would be ceremony until there is
 * something to put in one - a candidate gallery with turntables is the thing that would earn it.
 */
UCLASS()
class UMeshDefAssetDefinition : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:

	virtual FText GetAssetDisplayName() const override;
	virtual FLinearColor GetAssetColor() const override;
	virtual TSoftClassPtr<UObject> GetAssetClass() const override;
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override;

	/** Opens the four-tab toolkit rather than a details panel. See FMeshDefEditorToolkit. */
	virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
};

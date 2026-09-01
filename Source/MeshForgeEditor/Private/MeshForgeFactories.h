// Copyright Blackcode SA. All rights reserved.
//
// What makes a Mesh Definition appear under right-click > Automation Forge in the Content Browser.
//
// Without a factory a UDataAsset can still be created - through Miscellaneous > Data Asset, then
// picking the class out of a list of every data asset class in the project. That is the path a
// person finds after being told it exists, which is to say not at all. An asset definition decides
// what a double-click opens; a factory is what decides the thing can be made in the first place.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "MeshForgeFactories.generated.h"

/**
 * Creates a Mesh Definition, configured the way the pipeline creates one.
 *
 * The project defaults - provider, model, finish settings - are applied here as well as in
 * CreateMeshDef, through the same call, so a definition made by hand is not subtly different from
 * one an agent authored.
 */
UCLASS()
class UMeshDefFactory : public UFactory
{
	GENERATED_BODY()

public:

	UMeshDefFactory();

	virtual UObject* FactoryCreateNew(
		UClass* Class,
		UObject* InParent,
		FName Name,
		EObjectFlags Flags,
		UObject* Context,
		FFeedbackContext* Warn) override;

	virtual FText GetDisplayName() const override;
	virtual FString GetDefaultNewAssetName() const override;
};

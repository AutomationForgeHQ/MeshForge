// Copyright Blackcode SA. All rights reserved.

#include "MeshForgeFactories.h"

#include "MeshDef.h"
#include "MeshForgeSettings.h"

#define LOCTEXT_NAMESPACE "MeshForgeEditor"

UMeshDefFactory::UMeshDefFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = UMeshDef::StaticClass();
}

UObject* UMeshDefFactory::FactoryCreateNew(
	UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject*, FFeedbackContext*)
{
	UMeshDef* Def = NewObject<UMeshDef>(InParent, Class, Name, Flags);

	if (Def)
	{
		// The finish settings are applied here rather than inside ApplyProjectDefaults, and this is
		// the only honest place for them: nothing distinguishes a field nobody chose from one
		// somebody set to the same value, so the project's default can only be applied before there
		// is anybody to have chosen. A brand new asset is exactly that moment.
		Def->Finish = GetDefault<UMeshForgeSettings>()->DefaultFinish;

		// The same defaults CreateMeshDef applies, through the same call. A definition made by hand
		// that skipped this would look identical in the Content Browser and generate against nothing.
		Def->ApplyProjectDefaults();
	}

	return Def;
}

FText UMeshDefFactory::GetDisplayName() const
{
	return LOCTEXT("NewMeshDef", "Mesh Definition");
}

FString UMeshDefFactory::GetDefaultNewAssetName() const
{
	// MSD_ rather than MD_, which MotionForge already uses for a Motion Definition. Two asset types
	// sharing a prefix in one project is a small thing that makes a content browser unreadable.
	return TEXT("MSD_NewMesh");
}

#undef LOCTEXT_NAMESPACE

// Copyright Blackcode SA. All rights reserved.

#include "MeshDefDetails.h"
#include "MeshPipelineDetails.h"
#include "MeshDef.h"
#include "MeshForgePipeline.h"
#include "MeshDefThumbnailRenderer.h"

#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UObject/Package.h"

/**
 * MeshForge's editor presentation.
 *
 * Asset definitions and factories are discovered by their base classes and need no registration, so
 * the one thing here is the definition's details customization - which exists only to put its
 * categories in the order the pipeline runs. This module is separate from MeshForge itself because
 * the pipeline has no business depending on the Content Browser.
 *
 * The second customization shows a mesh pipeline which pictures it will be handed - read-only,
 * because choosing them stays in the Images tab where the gallery is.
 *
 * And a thumbnail renderer, so a definition's Content Browser tile shows its mesh or its main image.
 */
class FMeshForgeEditorModule : public IModuleInterface
{
public:

	virtual void StartupModule() override
	{
		FPropertyEditorModule& PropertyEditor =
			FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

		PropertyEditor.RegisterCustomClassLayout(
			UMeshDef::StaticClass()->GetFName(),
			FOnGetDetailCustomizationInstance::CreateStatic(&FMeshDefDetails::MakeInstance));

		// Registered on the base class, so every mesh pipeline gets it - including one written in a
		// provider add-on that this module has never heard of. The property editor walks up the
		// hierarchy looking for a customization, which is what makes that work.
		PropertyEditor.RegisterCustomClassLayout(
			UMeshForgePipeline::StaticClass()->GetFName(),
			FOnGetDetailCustomizationInstance::CreateStatic(&FMeshPipelineDetails::MakeInstance));

		// A definition's tile shows what it made, or the picture it is made from, instead of the class icon.
		UThumbnailManager::Get().RegisterCustomRenderer(UMeshDef::StaticClass(), UMeshDefThumbnailRenderer::StaticClass());
		PackageDirtiedHandle = UPackage::PackageMarkedDirtyEvent.AddStatic(&UMeshDefThumbnailRenderer::MarkThumbnailStale);
	}

	FDelegateHandle PackageDirtiedHandle;

	virtual void ShutdownModule() override
	{
		if (FPropertyEditorModule* PropertyEditor =
				FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
		{
			PropertyEditor->UnregisterCustomClassLayout(UMeshDef::StaticClass()->GetFName());
			PropertyEditor->UnregisterCustomClassLayout(UMeshForgePipeline::StaticClass()->GetFName());
		}

		UPackage::PackageMarkedDirtyEvent.Remove(PackageDirtiedHandle);

		if (UObjectInitialized())
		{
			UThumbnailManager::Get().UnregisterCustomRenderer(UMeshDef::StaticClass());
		}
	}
};

IMPLEMENT_MODULE(FMeshForgeEditorModule, MeshForgeEditor)

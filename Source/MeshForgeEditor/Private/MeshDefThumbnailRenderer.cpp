#include "MeshDefThumbnailRenderer.h"

#include "MeshDef.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "Misc/ObjectThumbnail.h"
#include "ObjectTools.h"
#include "RHIGlobals.h"
#include "SkinnedAssetCompiler.h"
#include "StaticMeshCompiler.h"
#include "ThumbnailRendering/ThumbnailManager.h"

#define LOCTEXT_NAMESPACE "MeshForgeEditor"

namespace MeshDefThumbnail
{
	/**
	 * The renderer Unreal uses for this object's own tile, unless it is ours - that would recurse.
	 *
	 * **Found from the manager's list rather than through GetRenderingInfo**, which also asks the renderer
	 * whether it can show the object *right now* and answers null when not. The skeletal mesh renderer says
	 * no for as long as a mesh is building, and a skinned garment loaded a moment ago always is - so the
	 * lookup failed before this renderer could wait for the build. Same search as the manager's: newest
	 * registration first, first class match wins.
	 */
	static UThumbnailRenderer* RendererFor(UObject* Subject)
	{
		if (Subject == nullptr || Subject->IsA<UMeshDef>())
		{
			return nullptr;
		}

		UThumbnailManager& Manager = UThumbnailManager::Get();

		if (const FArrayProperty* List = CastField<FArrayProperty>(
				UThumbnailManager::StaticClass()->FindPropertyByName(TEXT("RenderableThumbnailTypes"))))
		{
			FScriptArrayHelper Entries(List, List->ContainerPtrToValuePtr<void>(&Manager));
			for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
			{
				const FThumbnailRenderingInfo* Info = static_cast<const FThumbnailRenderingInfo*>(
					static_cast<void*>(Entries.GetRawPtr(Index)));
				const UClass* Wanted = Info->ClassNeedingThumbnail.Get();
				if (Info->Renderer != nullptr && Wanted != nullptr && Subject->GetClass()->IsChildOf(Wanted))
				{
					return Info->Renderer;
				}
			}
		}

		// The list moved in an engine update: fall back to the gated lookup, which still works for anything ready.
		const FThumbnailRenderingInfo* Info = Manager.GetRenderingInfo(Subject);
		return Info ? Info->Renderer : nullptr;
	}

	static bool IsMesh(const UObject* Object)
	{
		return Object != nullptr && (Object->IsA<UStaticMesh>() || Object->IsA<USkeletalMesh>());
	}

	static void FinishMaterials(const TArray<UMaterialInterface*>& Materials)
	{
		for (UMaterialInterface* Material : Materials)
		{
			if (Material == nullptr)
			{
				continue;
			}

			TArray<UTexture*> Textures;
			Material->GetUsedTextures(Textures);
			for (UTexture* Texture : Textures)
			{
				if (Texture != nullptr)
				{
					Texture->BlockOnAnyAsyncBuild();
				}
			}

			if (FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
			{
				if (!Resource->IsGameThreadShaderMapComplete())
				{
					Resource->SubmitCompileJobs_GameThread(EShaderCompileJobPriority::High);
					Resource->FinishCompilation();
				}
			}
		}
	}

	/**
	 * Finish whatever the subject is still building, so drawing it draws something.
	 *
	 * **This is why saved tiles came out empty.** The engine renders a definition's thumbnail while saving
	 * it, and waits only for a texture or material that is itself the thing being saved. The mesh this
	 * renderer loads has usually only just been loaded, still building its render data in the background,
	 * so the render drew nothing and the package stored a transparent image - a checkerboard in the
	 * Content Browser after the next restart. Each wait below returns at once when there is nothing to do.
	 */
	static void MakeReady(UObject* Subject)
	{
		if (UStaticMesh* Static = Cast<UStaticMesh>(Subject))
		{
			if (Static->IsCompiling())
			{
				FStaticMeshCompilingManager::Get().FinishCompilation({ Static });
			}
			TArray<UMaterialInterface*> Materials;
			for (const FStaticMaterial& Slot : Static->GetStaticMaterials()) Materials.Add(Slot.MaterialInterface);
			FinishMaterials(Materials);
		}
		else if (USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Subject))
		{
			if (Skeletal->IsCompiling())
			{
				FSkinnedAssetCompilingManager::Get().FinishCompilation({ Skeletal });
			}
			TArray<UMaterialInterface*> Materials;
			for (const FSkeletalMaterial& Slot : Skeletal->GetMaterials()) Materials.Add(Slot.MaterialInterface);
			FinishMaterials(Materials);
		}
		else if (UTexture* Texture = Cast<UTexture>(Subject))
		{
			Texture->BlockOnAnyAsyncBuild();
			Texture->WaitForStreaming();
		}
	}

}

UObject* UMeshDefThumbnailRenderer::ResolveSubject(const UMeshDef* Def)
{
	if (Def == nullptr)
	{
		return nullptr;
	}

	// The class is read from the asset registry, so only the one that is drawn gets loaded: earlier
	// outputs can be million-vertex meshes nobody asked to open.
	const IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// The newest mesh a post step produced. Newest rather than "the skeletal one", because a garment
	// chain ends in a skinned mesh while a retexture chain ends in a static one, and the last thing made
	// is the thing somebody is looking for. Steps that make other assets - an inventory item - are skipped.
	for (int32 Index = Def->PostOutputs.Num() - 1; Index >= 0; --Index)
	{
		const TSoftObjectPtr<UObject>& Asset = Def->PostOutputs[Index].Asset;
		const FAssetData Data = Asset.IsNull() ? FAssetData() : Registry.GetAssetByObjectPath(Asset.ToSoftObjectPath());

		if (Data.IsValid()
			&& (Data.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName()
				|| Data.AssetClassPath == USkeletalMesh::StaticClass()->GetClassPathName()))
		{
			if (UObject* Loaded = Asset.LoadSynchronous(); MeshDefThumbnail::IsMesh(Loaded))
			{
				return Loaded;
			}
		}
	}

	if (USkeletalMesh* Skeletal = Def->ImportedSkeletalMesh.LoadSynchronous())
	{
		return Skeletal;
	}

	if (UStaticMesh* Static = Def->ImportedMesh.LoadSynchronous())
	{
		return Static;
	}

	// Nothing made yet: the picture it will be made from.
	return Def->ResolveMainImage().LoadSynchronous();
}

bool UMeshDefThumbnailRenderer::CanVisualizeAsset(UObject* Object)
{
	UObject* Subject = ResolveSubject(Cast<UMeshDef>(Object));
	UThumbnailRenderer* Renderer = MeshDefThumbnail::RendererFor(Subject);
	if (Renderer == nullptr)
	{
		return false;
	}

	// Made ready before asking, not only before drawing: the skeletal mesh renderer answers "cannot show
	// this" for as long as the mesh is still building, and a skinned garment that has just been loaded
	// always is - so every skinned definition was saved with an empty thumbnail and showed the class icon.
	MeshDefThumbnail::MakeReady(Subject);

	// A skeletal mesh is judged here rather than by its renderer. The renderer also refuses while any of
	// the mesh's materials is compiling for *any* feature level at the active quality - a mobile preview
	// permutation nobody draws a thumbnail with - and a freshly imported garment material usually is. Its
	// render data and this platform's shaders are what the thumbnail draws with, and MakeReady finished both.
	if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Subject))
	{
		return !Skeletal->IsCompiling() && Skeletal->GetResourceForRendering() != nullptr;
	}

	return Renderer->CanVisualizeAsset(Subject);
}

void UMeshDefThumbnailRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height,
	FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily)
{
	UObject* Subject = ResolveSubject(Cast<UMeshDef>(Object));

	if (UThumbnailRenderer* Renderer = MeshDefThumbnail::RendererFor(Subject))
	{
		MeshDefThumbnail::MakeReady(Subject);
		Renderer->Draw(Subject, X, Y, Width, Height, RenderTarget, Canvas, bAdditionalViewFamily);
	}
}

void UMeshDefThumbnailRenderer::MarkThumbnailStale(UPackage* Package, bool /*bWasDirty*/)
{
	// A definition that changed may now show something else - a new take, a skinned garment - and a
	// thumbnail cached earlier in the session is otherwise saved again unchanged, because the save only
	// redraws a thumbnail that is missing, empty or dirty.
	if (Package == nullptr)
	{
		return;
	}

	if (UMeshDef* Def = Cast<UMeshDef>(Package->FindAssetInPackage()))
	{
		if (FObjectThumbnail* Thumbnail = ThumbnailTools::GetThumbnailForObject(Def))
		{
			Thumbnail->MarkAsDirty();
		}
	}
}

#undef LOCTEXT_NAMESPACE

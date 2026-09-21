// Repairing Mesh Definition thumbnails that were saved empty, or never saved.

#include "MeshForgeSubsystem.h"

#include "MeshDef.h"
#include "MeshForge.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FileHelpers.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/ScopedSlowTask.h"
#include "ObjectTools.h"
#include "ThumbnailRendering/ThumbnailManager.h"

#define LOCTEXT_NAMESPACE "MeshForge"

namespace MeshForgeThumbnails
{
	/**
	 * How many pixels of a rendered thumbnail hold anything. Zero is the empty, checkerboard tile.
	 *
	 * Colour counts as well as alpha: a mesh render writes alpha where the mesh is, but a picture drawn
	 * onto the canvas leaves alpha at zero, and counting alpha alone reported every image tile as empty.
	 */
	static int32 CoveredPixels(const FObjectThumbnail* Thumbnail)
	{
		if (Thumbnail == nullptr || Thumbnail->IsEmpty())
		{
			return 0;
		}

		const TArray<uint8>& Pixels = Thumbnail->GetUncompressedImageData();
		int32 Covered = 0;
		for (int32 Index = 0; Index + 3 < Pixels.Num(); Index += 4)   // BGRA
		{
			Covered += (Pixels[Index] | Pixels[Index + 1] | Pixels[Index + 2] | Pixels[Index + 3]) != 0 ? 1 : 0;
		}
		return Covered;
	}
}

FString UMeshForgeSubsystem::RebuildDefinitionThumbnails(const FString& Folder)
{
	const FString Root = Folder.IsEmpty() ? FString(TEXT("/Game")) : Folder;

	FARFilter Filter;
	Filter.ClassPaths.Add(UMeshDef::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(*Root));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Assets);

	FScopedSlowTask Progress(static_cast<float>(Assets.Num()),
		LOCTEXT("RebuildingThumbnails", "Rebuilding Mesh Definition thumbnails..."));
	Progress.MakeDialog(/*bShowCancelButton*/ true);

	TArray<UPackage*> ToSave;
	TArray<FString> EmptyNames;
	TArray<FString> NothingNames;
	int32 Drawn = 0, Nothing = 0;

	for (const FAssetData& Asset : Assets)
	{
		if (Progress.ShouldCancel())
		{
			break;
		}
		Progress.EnterProgressFrame(1.0f, FText::FromName(Asset.AssetName));

		UMeshDef* Def = Cast<UMeshDef>(Asset.GetAsset());
		if (Def == nullptr)
		{
			continue;
		}

		// Dirty first: MeshForgeEditor marks a definition's cached thumbnail stale when its package is
		// dirtied, and doing it afterwards would throw away the thumbnail about to be drawn.
		Def->MarkPackageDirty();

		// The renderer MeshForgeEditor registers decides what a definition shows; asked through the
		// thumbnail manager so this module needs no link to it. No renderer, or nothing to show, is an
		// empty thumbnail on purpose - drawn as the class icon, not as a checkerboard.
		const FThumbnailRenderingInfo* Info = UThumbnailManager::Get().GetRenderingInfo(Def);
		if (Info == nullptr || Info->Renderer == nullptr || !Info->Renderer->CanVisualizeAsset(Def))
		{
			ThumbnailTools::CacheEmptyThumbnail(Def->GetFullName(), Def->GetPackage());
			NothingNames.Add(Def->GetName());
			++Nothing;
		}
		else if (MeshForgeThumbnails::CoveredPixels(ThumbnailTools::GenerateThumbnailForObjectToSaveToDisk(Def)) > 0)
		{
			++Drawn;
		}
		else
		{
			EmptyNames.Add(Def->GetName());
		}

		ToSave.Add(Def->GetPackage());
	}

	UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty*/ false);

	FString Summary = FString::Printf(
		TEXT("Rebuilt Mesh Definition thumbnails under %s: %d drawn, %d with nothing made yet (class icon), %d rendered empty. %d saved."),
		*Root, Drawn, Nothing, EmptyNames.Num(), ToSave.Num());

	if (NothingNames.Num() > 0)
	{
		Summary += TEXT(" Nothing to show: ") + FString::Join(NothingNames, TEXT(", ")) + TEXT(".");
	}

	if (EmptyNames.Num() > 0)
	{
		Summary += TEXT(" Empty: ") + FString::Join(EmptyNames, TEXT(", ")) + TEXT(".");
	}

	UE_LOG(LogMeshForge, Display, TEXT("%s"), *Summary);
	return Summary;
}

#undef LOCTEXT_NAMESPACE

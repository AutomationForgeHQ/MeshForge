#include "MeshExporter.h"

#include "MeshForge.h"

#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "Exporters/GLTFExporter.h"
#include "Options/GLTFExportOptions.h"

FMeshExportResult FMeshExporter::ToGlb(
	UStaticMesh* Mesh, const FString& AbsolutePath, const FMeshExportOptions& Options)
{
	FMeshExportResult Result;

	// Said rather than asserted, because this is reachable from a button and from an agent tool,
	// and a check() on a background caller is a crash where a sentence would do.
	if (!IsInGameThread())
	{
		Result.Error = TEXT("A mesh can only be exported from the game thread.");
		return Result;
	}

	if (Mesh == nullptr)
	{
		Result.Error = TEXT("No mesh to export.");
		return Result;
	}

	if (AbsolutePath.IsEmpty())
	{
		Result.Error = TEXT("No path to export to.");
		return Result;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), true);

	// Built here rather than taken from the project's own glTF settings, and that is the point of
	// the struct above: those settings belong to whoever last exported something by hand, and a
	// pipeline whose upload size depends on that is a pipeline that fails for one person only.
	UGLTFExportOptions* Export = NewObject<UGLTFExportOptions>();

	Export->ExportUniformScale  = Options.UniformScale;
	Export->DefaultLevelOfDetail = FMath::Max(0, Options.LevelOfDetail);

	// Off unless asked for. Baking an Unreal material down to textures is minutes of work to produce
	// megabytes that a texturing service discards on arrival.
	Export->BakeMaterialInputs = Options.bIncludeTextures
		? EGLTFMaterialBakeMode::Simple
		: EGLTFMaterialBakeMode::Disabled;

	Export->TextureImageFormat = Options.bIncludeTextures
		? EGLTFTextureImageFormat::PNG
		: EGLTFTextureImageFormat::None;

	// Nothing here is a level or an animation, and every one of these costs time on a mesh that has
	// none of them anyway.
	Export->bExportPreviewMesh       = false;
	Export->bExportLevelSequences    = false;
	Export->bExportAnimationSequences = false;
	Export->bExportLights            = false;
	Export->bExportCameras           = false;

	// Kept: a hand-modelled prop can carry deliberate vertex colours, and a retexture that keeps the
	// original UVs is exactly the case where the rest of the mesh's authored data matters.
	Export->bExportVertexColors = true;

	FGLTFExportMessages Messages;

	const bool bExported = UGLTFExporter::ExportToGLTF(
		Mesh, AbsolutePath, Export, TSet<AActor*>(), Messages);

	Result.Warnings = Messages.Warnings;

	if (!bExported)
	{
		Result.Error = Messages.Errors.Num() > 0
			? FString::Join(Messages.Errors, TEXT(" "))
			: FString::Printf(TEXT("The glTF exporter refused '%s' without saying why."),
				*Mesh->GetName());
		return Result;
	}

	Result.SizeBytes = IFileManager::Get().FileSize(*AbsolutePath);

	// A successful export that wrote nothing is the failure worth catching here: everything
	// downstream treats an empty file as a mesh and fails somewhere far less obvious.
	if (Result.SizeBytes <= 0)
	{
		Result.Error = FString::Printf(
			TEXT("The glTF exporter reported success but wrote no file at '%s'."), *AbsolutePath);
		return Result;
	}

	Result.bSuccess = true;
	Result.Path     = AbsolutePath;

	UE_LOG(LogMeshForge, Log, TEXT("Exported '%s' to '%s' (%.1f MB, %s)."),
		*Mesh->GetName(), *AbsolutePath,
		static_cast<double>(Result.SizeBytes) / (1024.0 * 1024.0),
		Options.bIncludeTextures ? TEXT("with textures") : TEXT("geometry only"));

	return Result;
}

FMeshExportResult FMeshExporter::ToGlbBytes(
	UStaticMesh* Mesh, const FString& AbsolutePath, TArray<uint8>& OutBytes,
	const FMeshExportOptions& Options)
{
	OutBytes.Reset();

	FMeshExportResult Result = ToGlb(Mesh, AbsolutePath, Options);

	if (!Result.bSuccess)
	{
		return Result;
	}

	if (!FFileHelper::LoadFileToArray(OutBytes, *Result.Path))
	{
		Result.bSuccess = false;
		Result.Error = FString::Printf(
			TEXT("Exported '%s' but could not read it back."), *Result.Path);
	}

	return Result;
}

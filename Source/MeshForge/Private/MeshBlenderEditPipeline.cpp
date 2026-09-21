#include "MeshBlenderEditPipeline.h"

#include "MeshExporter.h"
#include "MeshPositionMap.h"
#include "MeshForge.h"
#include "MeshForgeEditorSettings.h"
#include "MeshForgeSettings.h"

#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "MeshDescription.h"
#include "Misc/Paths.h"
#include "SkeletalMeshTypes.h"
#include "StaticMeshAttributes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace MeshBlenderEdit
{
	static const TCHAR* ManifestName = TEXT("manifest.json");
	static const TCHAR* PositionsName = TEXT("positions.bin");

	static bool ReadJson(const FString& Path, TSharedPtr<FJsonObject>& Out)
	{
		FString Text;
		return FFileHelper::LoadFileToString(Text, *Path)
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) && Out.IsValid();
	}
}

FString UMeshBlenderEditPipeline::ResolveScript()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MeshForge"));
	if (!Plugin.IsValid())
	{
		return FString();
	}

	const FString Script = FPaths::ConvertRelativePathToFull(
		Plugin->GetBaseDir() / TEXT("Scripts/Blender/meshforge_round_trip.py"));
	return FPaths::FileExists(Script) ? Script : FString();
}

FString UMeshBlenderEditPipeline::Validate() const
{
	if (UMeshForgeEditorSettings::Get()->ResolveBlender().IsEmpty())
	{
		return TEXT("Blender was not found. Install it, or point Editor Preferences > Automation Forge > MeshForge > Blender Executable at it.");
	}

	if (ResolveScript().IsEmpty())
	{
		return TEXT("MeshForge's Blender add-on (Scripts/Blender/meshforge_round_trip.py) is missing from the plugin. Reinstall MeshForge.");
	}

	return FString();
}

void UMeshBlenderEditPipeline::Run(const FMeshPostJob& Job, FMeshPostResult& OutResult) const
{
	// Never reached: the chain hands an interactive step to BeginInteractive instead of a worker.
	OutResult.Error = TEXT("Edit in Blender runs interactively and cannot run on a worker.");
}

bool UMeshBlenderEditPipeline::BeginInteractive(const FMeshPostInteractiveSession& Session, FString& OutError) const
{
	OutError = Validate();
	if (!OutError.IsEmpty())
	{
		return false;
	}

	// --- a skinned mesh goes out with its skeleton, weights and morph targets ------------------------
	//
	// Exported here rather than by the chain, which writes glTF for static meshes only, and with the textures:
	// this file is for a person to look at.
	if (bEditSkinnedMesh)
	{
		USkeletalMesh* Skinned = Cast<USkeletalMesh>(FSoftObjectPath(Session.InputAssetPath).TryLoad());
		if (Skinned == nullptr)
		{
			OutError = FString::Printf(TEXT("'Edit skinned mesh' is on, and this step's input ('%s') is not a skeletal mesh. "
				"Point it at the skinning step's output, or turn the setting off to edit a static mesh."), *Session.InputAssetPath);
			return false;
		}

		FMeshExportOptions Options;
		Options.bIncludeTextures = true;
		const FMeshExportResult Exported = FMeshExporter::ToGlb(Skinned, Session.InputGlb, Options);
		if (!Exported.bSuccess)
		{
			OutError = Exported.Error;
			return false;
		}
	}

	// --- references, in the same space as the mesh -------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> References;

	for (int32 Index = 0; Index < ReferenceMeshes.Num(); ++Index)
	{
		UObject* Reference = ReferenceMeshes[Index].LoadSynchronous();
		if (Reference == nullptr)
		{
			continue;
		}

		const FString File = FString::Printf(TEXT("reference_%d.glb"), Index);
		const FString Path = Session.Directory / File;

		FMeshExportResult Exported;
		if (UStaticMesh* Static = Cast<UStaticMesh>(Reference))
		{
			Exported = FMeshExporter::ToGlb(Static, Path);
		}
		else if (USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Reference))
		{
			Exported = FMeshExporter::ToGlb(Skeletal, Path);
		}

		if (!Exported.bSuccess)
		{
			// A missing reference is worth a warning, not a refusal: the mesh can still be edited.
			UE_LOG(LogMeshForge, Warning, TEXT("Could not export reference '%s' for Blender: %s"),
				*Reference->GetName(), *Exported.Error);
			continue;
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("file"), File);
		Entry->SetStringField(TEXT("name"), Reference->GetName());
		References.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// --- what the add-on reads ----------------------------------------------------------------------
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetNumberField(TEXT("version"), 1);
	Request->SetStringField(TEXT("session"), Session.Id.ToString(EGuidFormats::DigitsWithHyphensLower));
	Request->SetStringField(TEXT("definition"), Session.DefinitionName);
	Request->SetStringField(TEXT("input"), FPaths::GetCleanFilename(Session.InputGlb));
	Request->SetArrayField(TEXT("references"), References);
	Request->SetStringField(TEXT("result"), TEXT("result.glb"));
	Request->SetStringField(TEXT("manifest"), MeshBlenderEdit::ManifestName);
	Request->SetBoolField(TEXT("sculpt"), bOpenInSculptMode);
	if (bEditSkinnedMesh)
	{
		// What comes back is put on this asset's own vertices, so the add-on reports where each of them went.
		Request->SetStringField(TEXT("positions"), MeshBlenderEdit::PositionsName);
		Request->SetBoolField(TEXT("skinned"), true);
	}


	FString Text;
	FJsonSerializer::Serialize(Request, TJsonWriterFactory<>::Create(&Text));
	const FString RequestPath = Session.Directory / TEXT("blender-session.json");

	if (!FFileHelper::SaveStringToFile(Text, *RequestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Could not write '%s'."), *RequestPath);
		return false;
	}

	// --- and open Blender on it ---------------------------------------------------------------------
	const FString Blender = UMeshForgeEditorSettings::Get()->ResolveBlender();
	const FString Params = FString::Printf(TEXT("--python \"%s\" -- --meshforge-session \"%s\""),
		*ResolveScript(), *RequestPath);

	// Detached and visible: this is a window for a person, which lives on after Unreal stops watching.
	FProcHandle Process = FPlatformProcess::CreateProc(*Blender, *Params,
		/*bLaunchDetached*/ true, /*bLaunchHidden*/ false, /*bLaunchReallyHidden*/ false,
		nullptr, 0, *Session.Directory, nullptr);

	if (!Process.IsValid())
	{
		OutError = FString::Printf(TEXT("Blender could not be started from '%s'."), *Blender);
		return false;
	}

	FPlatformProcess::CloseProc(Process);

	UE_LOG(LogMeshForge, Log, TEXT("Opened '%s' in Blender (%s). Press Send back to Unreal in Blender's MeshForge tab when done."),
		*Session.DefinitionName, *Blender);
	return true;
}

EMeshInteractiveState UMeshBlenderEditPipeline::PollInteractive(const FMeshPostInteractiveSession& Session,
	FString& OutResultGlb, FString& OutSummary, FString& OutError) const
{
	const FString ManifestPath = Session.Directory / MeshBlenderEdit::ManifestName;

	if (!FPaths::FileExists(ManifestPath))
	{
		return EMeshInteractiveState::Waiting;
	}

	TSharedPtr<FJsonObject> Manifest;
	if (!MeshBlenderEdit::ReadJson(ManifestPath, Manifest))
	{
		// Written by rename, so a partial file should never be seen. If it is, it is not ours to judge yet.
		return EMeshInteractiveState::Waiting;
	}

	const FString Status = Manifest->GetStringField(TEXT("status"));

	if (Status == TEXT("cancelled"))
	{
		OutError = TEXT("The edit was cancelled in Blender. Nothing was imported.");
		return EMeshInteractiveState::Failed;
	}

	OutResultGlb = Session.Directory / Manifest->GetStringField(TEXT("result"));

	if (!FPaths::FileExists(OutResultGlb))
	{
		OutError = FString::Printf(TEXT("Blender's manifest names '%s', which is not there."), *OutResultGlb);
		return EMeshInteractiveState::Failed;
	}

	const int32 Before = static_cast<int32>(Manifest->GetNumberField(TEXT("verticesBefore")));
	const int32 After  = static_cast<int32>(Manifest->GetNumberField(TEXT("verticesAfter")));
	const bool bTopologyChanged = Manifest->GetBoolField(TEXT("topologyChanged"));

	if (bEditSkinnedMesh)
	{
		if (bTopologyChanged)
		{
			OutError = FString::Printf(
				TEXT("The edit changed the topology (%d vertices and %d faces, was %d and %d). A skinned mesh comes back by moving "
					 "its own vertices, which is what keeps its weights and morph targets, so its topology has to come back as it "
					 "went. Undo what added or removed geometry in Blender and send again, or edit the static mesh before skinning "
					 "instead."),
				After, static_cast<int32>(Manifest->GetNumberField(TEXT("facesAfter"))),
				Before, static_cast<int32>(Manifest->GetNumberField(TEXT("facesBefore"))));
			return EMeshInteractiveState::Failed;
		}

		if (!Manifest->HasField(TEXT("positions")))
		{
			OutError = TEXT("Blender sent the mesh back without saying where its vertices went, so the edit cannot be put on the "
							"skinned asset. Send again from the MeshForge tab; if it keeps happening, the add-on and this plugin "
							"are different versions.");
			return EMeshInteractiveState::Failed;
		}

		OutSummary = FString::Printf(TEXT("Edited in Blender as a skinned mesh: %d vertices, topology unchanged."), After);
		return EMeshInteractiveState::Done;
	}
	// Said plainly, because a changed topology silently breaks a retexture's UVs and a morph bake's
	// vertex order further down, while skinning carries on regardless.
	OutSummary = bTopologyChanged
		? FString::Printf(TEXT("Edited in Blender: %d vertices, was %d. Topology changed - retexture and morph baking will not line up with the original."), After, Before)
		: FString::Printf(TEXT("Edited in Blender: %d vertices, topology unchanged."), After);

	return EMeshInteractiveState::Done;
}

bool UMeshBlenderEditPipeline::AcceptsNativeInput(const UClass* InputClass) const
{
	// Native only while it edits a skinned mesh, and then only a skeletal mesh will do.
	return bEditSkinnedMesh && InputClass != nullptr && InputClass->IsChildOf(USkeletalMesh::StaticClass());
}

UObject* UMeshBlenderEditPipeline::CreateNativeOutput(UObject* Input, FMeshPostNativeContext& Context, FString& Error) const
{
	USkeletalMesh* Source = Cast<USkeletalMesh>(Input);
	if (!bEditSkinnedMesh || Source == nullptr)
	{
		Error = TEXT("Edit in Blender makes an asset only when it edits a skinned mesh, and then only from a skeletal mesh.");
		return nullptr;
	}

	// Where Blender left each vertex, against where it found it.
	TArray<FVector3f> Before, After;
	const FString PositionsPath = InteractiveSessionDirectory / MeshBlenderEdit::PositionsName;
	if (!FMeshPositionMap::ReadFile(PositionsPath, Before, After))
	{
		Error = FString::Printf(TEXT("Blender's vertex positions ('%s') are missing or unreadable, so the edit cannot be put on the skinned mesh."),
			*PositionsPath);
		return nullptr;
	}

	const FMeshDescription* SourceDescription = Source->GetMeshDescription(0);
	if (SourceDescription == nullptr)
	{
		Error = FString::Printf(TEXT("'%s' has no editable mesh at LOD 0."), *Source->GetName());
		return nullptr;
	}

	TArray<FVector3f> Snapshot;
	{
		const TVertexAttributesConstRef<FVector3f> Positions =
			SourceDescription->VertexAttributes().GetAttributesRef<FVector3f>(MeshAttribute::Vertex::Position);
		Snapshot.SetNumZeroed(SourceDescription->Vertices().GetArraySize());
		for (const FVertexID Vertex : SourceDescription->Vertices().GetElementIDs())
		{
			Snapshot[Vertex.GetValue()] = Positions[Vertex];
		}
	}

	TArray<FVector3f> Edited;
	if (!FMeshPositionMap::Apply(Snapshot, Before, After, Edited, Error))
	{
		return nullptr;
	}

	// A duplicate of the asset itself, never a new import: that is what keeps the skeleton it is bound to, its
	// weights, its morph targets and its materials exactly as they were. See garment-binds-to-the-target-mesh.
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	USkeletalMesh* Output = Cast<USkeletalMesh>(AssetTools.DuplicateAsset(
		FPaths::GetBaseFilename(Context.AssetPath), FPaths::GetPath(Context.AssetPath), Source));
	if (Output == nullptr)
	{
		Error = FString::Printf(TEXT("Could not copy '%s' to '%s'."), *Source->GetName(), *Context.AssetPath);
		return nullptr;
	}

	int32 Moved = 0;
	{
		FScopedSkeletalMeshPostEditChange PostEditChange(Output);
		Output->Modify();
		Output->ModifyMeshDescription(0);

		FMeshDescription* EditedDescription = Output->GetMeshDescription(0);
		if (EditedDescription == nullptr || EditedDescription->Vertices().GetArraySize() != Edited.Num())
		{
			Error = FString::Printf(TEXT("'%s' does not have the vertices the edit was made on."), *Output->GetName());
			return nullptr;
		}

		TVertexAttributesRef<FVector3f> Positions =
			EditedDescription->VertexAttributes().GetAttributesRef<FVector3f>(MeshAttribute::Vertex::Position);
		for (const FVertexID Vertex : EditedDescription->Vertices().GetElementIDs())
		{
			const FVector3f& Position = Edited[Vertex.GetValue()];
			if (!Position.Equals(Positions[Vertex], 1.e-4f))
			{
				++Moved;
			}
			Positions[Vertex] = Position;
		}

		Output->CommitMeshDescription(0);
	}
	Output->Build();
	Output->MarkPackageDirty();

	if (UMeshForgeSettings::Get()->bSaveGeneratedAssets)
	{
		UPackage* Package = Output->GetOutermost();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(Package, nullptr,
			*FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}

	Context.Summary = FString::Printf(
		TEXT("Edited in Blender as a skinned mesh: %d of %d vertices moved. Skeleton, weights, morph targets and materials are '%s'."),
		Moved, Edited.Num(), *Source->GetName());
	UE_LOG(LogMeshForge, Log, TEXT("%s -> %s"), *Context.Summary, *Output->GetPathName());
	return Output;
}

void UMeshBlenderEditPipeline::ConsumeInteractiveResult(const FMeshPostInteractiveSession& Session) const
{
	// Renamed rather than deleted: the history of what was sent stays readable in the session folder.
	const FString ManifestPath = Session.Directory / MeshBlenderEdit::ManifestName;
	const FString Taken = Session.Directory / FString::Printf(TEXT("manifest.taken-%s.json"),
		*FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")));
	IFileManager::Get().Move(*Taken, *ManifestPath);
}

void UMeshBlenderEditPipeline::OnInteractiveOutputImported(UStaticMesh* Output, UObject* Input) const
{
	UStaticMesh* Source = Cast<UStaticMesh>(Input);

	if (!bRestoreInputMaterials || Output == nullptr || Source == nullptr)
	{
		return;
	}

	TArray<FStaticMaterial>& Materials = Output->GetStaticMaterials();
	const TArray<FStaticMaterial>& Originals = Source->GetStaticMaterials();

	if (Materials.Num() != Originals.Num())
	{
		UE_LOG(LogMeshForge, Warning,
			TEXT("'%s' came back from Blender with %d material slot(s) where the input had %d, so its materials were not restored. Assign them by hand."),
			*Output->GetName(), Materials.Num(), Originals.Num());
		return;
	}

	Output->Modify();
	for (int32 Index = 0; Index < Materials.Num(); ++Index)
	{
		Materials[Index].MaterialInterface = Originals[Index].MaterialInterface;
	}
	Output->PostEditChange();
	Output->MarkPackageDirty();

	if (UMeshForgeSettings::Get()->bSaveGeneratedAssets)
	{
		UPackage* Package = Output->GetOutermost();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(Package, nullptr,
			*FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}

	UE_LOG(LogMeshForge, Log, TEXT("Restored %d material slot(s) on '%s' from '%s'."),
		Materials.Num(), *Output->GetName(), *Source->GetName());
}

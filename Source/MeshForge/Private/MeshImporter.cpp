#include "MeshImporter.h"

#include "MeshForge.h"

#include "Algo/Count.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "IAssetTools.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshEditorSubsystem.h"
#include "StaticMeshEditorSubsystemHelpers.h"
#include "UObject/SavePackage.h"

namespace MeshImporterPrivate
{
	/**
	 * Run a glTF import through whatever Unreal has registered for it.
	 *
	 * Note what this deliberately does *not* do: force a factory, or switch Interchange off. The
	 * MotionForge importer has to hold Interchange back because it needs FBX options Interchange
	 * ignores; here Interchange is the *right* path - it is the only one in 5.8 that reads a GLB's
	 * materials and textures rather than only its geometry - so the default is what we want.
	 */
	static bool RunImport(UAssetImportTask* Task)
	{
		FAssetToolsModule& AssetTools =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

		TArray<UAssetImportTask*> Tasks;
		Tasks.Add(Task);

		AssetTools.Get().ImportAssetTasks(Tasks);

		return Task->GetObjects().Num() > 0 || Task->ImportedObjectPaths.Num() > 0;
	}

	/** Everything an import task produced, loaded, whichever way the task chose to report it. */
	static TArray<UObject*> GatherImported(UAssetImportTask* Task)
	{
		TArray<UObject*> Objects;

		for (UObject* Object : Task->GetObjects())
		{
			if (Object != nullptr)
			{
				Objects.AddUnique(Object);
			}
		}

		// Both, because the two disagree. GetObjects is empty on some Interchange paths that still
		// report paths, and relying on either alone loses assets intermittently - which reads as a
		// glTF with no textures rather than as a bookkeeping difference.
		for (const FString& Path : Task->ImportedObjectPaths)
		{
			if (UObject* Object = LoadObject<UObject>(nullptr, *Path))
			{
				Objects.AddUnique(Object);
			}
		}

		return Objects;
	}

	static void SavePackageFor(UObject* Object)
	{
		if (Object == nullptr)
		{
			return;
		}

		UPackage* Package = Object->GetOutermost();
		if (Package == nullptr || !Package->IsDirty())
		{
			return;
		}

		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;

		UPackage::SavePackage(Package, nullptr, *FileName, Args);
	}

	/**
	 * Shift and scale a mesh's geometry in place.
	 *
	 * Done on the mesh description rather than through BuildScale3D, and the difference is not
	 * cosmetic. BuildScale3D is applied at build time and is *not* reflected in the asset's own
	 * vertex data, so collision fitted afterwards is fitted to the unscaled shape and every bound
	 * reported by the editor is the old one. Editing the description means everything downstream -
	 * collision, bounds, lightmap packing - sees the mesh at the size it will actually be.
	 */
	static bool TransformGeometry(UStaticMesh* Mesh, float TargetSizeCm, bool bOriginAtBase, FVector& OutBoundsSize)
	{
		if (Mesh == nullptr || !Mesh->IsMeshDescriptionValid(0))
		{
			return false;
		}

		FMeshDescription* Description = Mesh->GetMeshDescription(0);
		if (Description == nullptr)
		{
			return false;
		}

		FStaticMeshAttributes Attributes(*Description);
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();

		if (Positions.GetNumElements() == 0)
		{
			return false;
		}

		FBox Bounds(ForceInit);
		for (const FVertexID VertexID : Description->Vertices().GetElementIDs())
		{
			Bounds += FVector(Positions[VertexID]);
		}

		// Scale is derived from the size somebody asked for, not multiplied in.
		//
		// **The mesh is already in centimetres by the time we see it.** Interchange applies glTF's
		// metre-to-Unreal-unit conversion during import, so a unit-box asset arrives 100 units
		// across, not 1. An earlier version of this multiplied by 100 on top of that and produced
		// props a hundred metres wide - which reads as a broken exporter rather than as the two
		// conversions it actually was. Measuring what is here and scaling to a target cannot make
		// that mistake, because it never assumes what the incoming size means.
		const FVector Size = Bounds.GetSize();
		const double Longest = FMath::Max3(Size.X, Size.Y, Size.Z);

		float Scale = 1.f;

		if (TargetSizeCm > 0.f && Longest > UE_SMALL_NUMBER)
		{
			Scale = static_cast<float>(TargetSizeCm / Longest);
		}

		// Centred on X and Y whether or not the origin moves vertically: a prop whose pivot is off to
		// one side rotates around a point outside itself, which is wrong in every case and is the
		// single most common complaint about generated meshes.
		FVector Offset(-Bounds.GetCenter().X, -Bounds.GetCenter().Y, -Bounds.GetCenter().Z);

		if (bOriginAtBase)
		{
			Offset.Z = -Bounds.Min.Z;
		}

		Mesh->ModifyMeshDescription(0);

		for (const FVertexID VertexID : Description->Vertices().GetElementIDs())
		{
			const FVector Moved = (FVector(Positions[VertexID]) + Offset) * Scale;
			Positions[VertexID] = FVector3f(Moved);
		}

		Mesh->CommitMeshDescription(0);

		OutBoundsSize = Bounds.GetSize() * Scale;
		return true;
	}

	static EScriptCollisionShapeType ToScriptShape(EMeshCollisionMode Mode)
	{
		switch (Mode)
		{
		case EMeshCollisionMode::Box:    return EScriptCollisionShapeType::Box;
		case EMeshCollisionMode::Sphere: return EScriptCollisionShapeType::Sphere;

		// There is no "single convex hull" in the scripting enum, and NDOP26 is the closest thing
		// available: a 26-sided discrete oriented polytope, which for a generated prop is a convex
		// hull in everything but name and is cheaper to build than one.
		default:                         return EScriptCollisionShapeType::NDOP26;
		}
	}
}

FMeshImportOutcome FMeshImporter::Import(const FMeshImportRequest& Request)
{
	FMeshImportOutcome Outcome;

	if (!FPaths::FileExists(Request.AbsoluteArtifactPath))
	{
		Outcome.Error = FString::Printf(TEXT("No file at '%s'."), *Request.AbsoluteArtifactPath);
		return Outcome;
	}

	if (Request.MeshPackagePath.IsEmpty() || Request.AssetName.IsEmpty())
	{
		Outcome.Error = TEXT("An import needs both a destination path and an asset name.");
		return Outcome;
	}

	const FString SafeName = ObjectTools::SanitizeObjectName(Request.AssetName);

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = Request.AbsoluteArtifactPath;
	Task->DestinationPath = Request.MeshPackagePath;
	Task->DestinationName = SafeName;
	Task->bAutomated = true;      // no modal dialog; this runs inside a batch
	Task->bReplaceExisting = true;
	Task->bSave = false;          // saving is decided below, once we know what was made
	Task->bAsync = false;

	if (!MeshImporterPrivate::RunImport(Task))
	{
		Outcome.Error = FString::Printf(
			TEXT("Interchange produced nothing from '%s'. The file may be an invalid glTF."),
			*Request.AbsoluteArtifactPath);
		return Outcome;
	}

	const TArray<UObject*> Imported = MeshImporterPrivate::GatherImported(Task);

	// Only the mesh is taken from what the import reported. Its materials and textures are read off
	// the mesh itself further down, because a re-import creates only what changed and reports only
	// what it created - so this list is not a description of the asset, only of the run.
	UStaticMesh* Mesh = nullptr;

	for (UObject* Object : Imported)
	{
		if (UStaticMesh* AsMesh = Cast<UStaticMesh>(Object))
		{
			// The first, deliberately. A generated prop is one object; a glTF carrying several means
			// the provider split it, and importing the rest as siblings is better than picking an
			// arbitrary one silently. Recorded as a warning below.
			if (Mesh == nullptr)
			{
				Mesh = AsMesh;
			}
		}
	}

	if (Mesh == nullptr)
	{
		Outcome.Error = FString::Printf(
			TEXT("Imported %d assets from '%s' but none was a static mesh."),
			Imported.Num(), *Request.AbsoluteArtifactPath);
		return Outcome;
	}

	const int32 MeshCount = static_cast<int32>(Algo::CountIf(Imported,
		[](const UObject* Object) { return Object != nullptr && Object->IsA<UStaticMesh>(); }));

	if (MeshCount > 1)
	{
		Outcome.Warnings.Add(FString::Printf(
			TEXT("The file contained %d meshes; '%s' was taken as the result and the rest were "
				 "imported beside it."),
			MeshCount, *Mesh->GetName()));
	}

	FMeshImportOutcome Finished = Refinish(Mesh, Request.Finish, Request.NaniteTriangleThreshold);

	Finished.Warnings.Append(Outcome.Warnings);

	if (Request.bSaveAssets)
	{
		MeshImporterPrivate::SavePackageFor(Mesh);

		for (const TSoftObjectPtr<UMaterialInterface>& Material : Finished.Materials)
		{
			MeshImporterPrivate::SavePackageFor(Material.Get());
		}

		for (const TSoftObjectPtr<UTexture>& Texture : Finished.Textures)
		{
			MeshImporterPrivate::SavePackageFor(Texture.Get());
		}
	}

	UE_LOG(LogMeshForge, Log,
		TEXT("Imported '%s': %d triangles, %d materials, %d textures, %d collision primitives, Nanite %s."),
		*Mesh->GetName(), Finished.TriangleCount,
		Finished.Materials.Num(), Finished.Textures.Num(),
		Finished.CollisionPrimitives, Finished.bNaniteEnabled ? TEXT("on") : TEXT("off"));

	return Finished;
}

FMeshImportOutcome FMeshImporter::Refinish(
	UStaticMesh* Mesh,
	const FMeshFinishSettings& Finish,
	int32 NaniteTriangleThreshold)
{
	FMeshImportOutcome Outcome;

	if (Mesh == nullptr)
	{
		Outcome.Error = TEXT("There is no mesh to finish.");
		return Outcome;
	}

	Outcome.Mesh = Mesh;

	UStaticMeshEditorSubsystem* MeshEditor =
		GEditor ? GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>() : nullptr;

	if (MeshEditor == nullptr)
	{
		Outcome.Error = TEXT("The static mesh editor subsystem is unavailable; nothing could be finished.");
		return Outcome;
	}

	// ---------------------------------------------------------------------------------------------
	// Geometry: scale, and where the pivot is
	// ---------------------------------------------------------------------------------------------

	if (!MeshImporterPrivate::TransformGeometry(
			Mesh, Finish.TargetSizeCm, Finish.bOriginAtBase, Outcome.BoundsSize))
	{
		Outcome.Warnings.Add(
			TEXT("Could not read the mesh geometry, so scale and pivot were left alone."));
	}

	// ---------------------------------------------------------------------------------------------
	// Build settings: the lightmap UV set
	// ---------------------------------------------------------------------------------------------

	FMeshBuildSettings Build;
	MeshEditor->GetLodBuildSettings(Mesh, 0, Build);

	// Read before deciding, because the answer depends on what actually arrived. A generated mesh
	// normally has exactly one UV set - the atlas its texture was baked into - and destination
	// channel 1 is therefore free. A provider that emitted two would have this overwrite one.
	Outcome.SourceUVChannels = Mesh->GetNumUVChannels(0);

	Build.bGenerateLightmapUVs = Finish.bGenerateLightmapUVs;
	Build.MinLightmapResolution = Finish.LightmapResolution;
	Build.SrcLightmapIndex = 0;
	Build.DstLightmapIndex = FMath::Max(1, Outcome.SourceUVChannels);

	// Left alone deliberately: the geometry was already scaled above, and doing it twice is a
	// mesh at ten thousand times its intended size.
	Build.BuildScale3D = FVector::OneVector;

	MeshEditor->SetLodBuildSettings(Mesh, 0, Build);

	if (Finish.bGenerateLightmapUVs)
	{
		Mesh->SetLightMapResolution(Finish.LightmapResolution);
		Mesh->SetLightMapCoordinateIndex(Build.DstLightmapIndex);
	}

	// ---------------------------------------------------------------------------------------------
	// Collision
	// ---------------------------------------------------------------------------------------------

	// Always cleared first. Interchange builds a fallback hull of its own on import, so without this
	// every re-finish adds another set on top of the last and a prop ends up with nine overlapping
	// hulls that all report as working collision.
	MeshEditor->RemoveCollisions(Mesh);

	switch (Finish.Collision)
	{
	case EMeshCollisionMode::None:
		break;

	case EMeshCollisionMode::ConvexDecomposition:
		// The last argument is hull *precision* - the voxel resolution the decomposer works at -
		// and it was 100 here, with a comment claiming that was the subsystem's default. It is not.
		// The Static Mesh editor's own constants are MinHullPrecision 10,000, DefaultHullPrecision
		// 100,000, MaxHullPrecision 1,000,000, so 100 was a hundred times below the lowest value the
		// UI will even offer.
		//
		// At that resolution the voxelisation is far too coarse to find the concavities it is
		// looking for. A small, simple mesh can still split - a 9,600-triangle Draft prop came back
		// with 8 hulls - but a detailed one collapses to a single hull around the whole shape,
		// which is what a 480,553-triangle generation did every time, however many were asked for.
		//
		// So the symptom is "decomposition stops working as the mesh gets good", which reads like a
		// property of the mesh rather than a wrong argument. Nothing reported it either, because the
		// subsystem only rejects a *negative* precision.
		if (!MeshEditor->SetConvexDecompositionCollisions(
				Mesh,
				FMath::Clamp(Finish.ConvexHullCount, 1, 32),
				FMath::Clamp(Finish.ConvexHullVertices, 6, 32),
				100000 /* DefaultHullPrecision, from StaticMeshEditorTools.cpp */))
		{
			Outcome.Warnings.Add(
				TEXT("Convex decomposition failed, so this mesh has no collision. A mesh with holes "
					 "or loose shells is the usual cause; try Single Convex."));
		}
		break;

	default:
		if (MeshEditor->AddSimpleCollisions(Mesh, MeshImporterPrivate::ToScriptShape(Finish.Collision))
			== INDEX_NONE)
		{
			Outcome.Warnings.Add(TEXT("Could not fit a simple collision primitive to this mesh."));
		}
		break;
	}

	// Both, because they are separate counts and convex decomposition produces only the second.
	// Reading one of them reported zero on a mesh that in fact had a hull around it, which is the
	// worst kind of wrong: a warning nobody gets, about a problem that is not there.
	Outcome.CollisionPrimitives =
		FMath::Max(0, MeshEditor->GetSimpleCollisionCount(Mesh))
		+ FMath::Max(0, MeshEditor->GetConvexCollisionCount(Mesh));

	// A decomposition that returns one hull did not decompose, and it does not report a failure to
	// say so - it is indistinguishable from success until somebody walks into the shape. This is the
	// check that would have caught a precision of 100 on the day it was written rather than months
	// later, so it stays even though the cause is fixed: the next wrong argument will be a different
	// one.
	//
	// **It deliberately does not claim which of the two things happened.** One hull is the correct
	// answer for a convex prop - a crate, a barrel - and a warning that asserts "handles and
	// recesses will feel solid" sends somebody to investigate a mesh that has neither.
	//
	// Telling them apart by comparing the hull's volume to the mesh's was tried and withdrawn: it
	// reported a generated crate as enclosing 402 times its own volume, which is impossible for a
	// shape 55x37x60cm, and a confident wrong number is worse than an honest either-or. The likely
	// cause is that a generated surface is not reliably closed, so signed tetrahedra mean nothing -
	// if this is picked up again, validate against a mesh known to be watertight first.
	if (Finish.Collision == EMeshCollisionMode::ConvexDecomposition
		&& Finish.ConvexHullCount > 1
		&& Outcome.CollisionPrimitives == 1)
	{
		Outcome.Warnings.Add(FString::Printf(
			TEXT("Convex decomposition produced 1 hull where %d were asked for. If this prop is "
				 "boxy, that is the correct answer and there is nothing to fix. If it has handles, "
				 "recesses or gaps under feet, they will feel solid - raise the hull count, or use "
				 "Single Convex and accept it."),
			Finish.ConvexHullCount));
	}

	// ---------------------------------------------------------------------------------------------
	// Nanite
	// ---------------------------------------------------------------------------------------------

	// The build has to happen before the triangle count is trustworthy, and the triangle count is
	// what Auto decides on - so this order is required rather than tidy.
	Mesh->Build(false);

	// Counts come from the mesh description, not from render data, and this is the third thing that
	// made these numbers wrong.
	//
	// `GetNumTriangles(0)` reads whatever build has completed at that instant, so it reported 22,762
	// for a mesh that has 486,127 and then reported the true figure minutes later. And once Nanite
	// is on it stops meaning what the field says at all: LOD 0 becomes the *fallback* mesh, so a
	// 486,127-triangle asset legitimately answers 22,762 forever after.
	//
	// The mesh description has neither problem. It is correct the moment it is written, it does not
	// change under Nanite, and it is directly comparable to what the provider said it produced -
	// 486,140 from the runner against 486,127 here, the difference being welded vertices.
	int32 SourceTriangles = 0;
	int32 SourceVertices = 0;

	if (const FMeshDescription* Description = Mesh->GetMeshDescription(0))
	{
		SourceTriangles = Description->Triangles().Num();
		SourceVertices = Description->Vertices().Num();
	}

	bool bNanite = false;

	switch (Finish.Nanite)
	{
	case EMeshNaniteMode::On:   bNanite = true; break;
	case EMeshNaniteMode::Off:  bNanite = false; break;
	case EMeshNaniteMode::Auto: bNanite = SourceTriangles >= NaniteTriangleThreshold; break;
	}

	// Through the accessor: the member is deprecated in 5.7 and becomes private.
	FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
	if (Nanite.bEnabled != bNanite)
	{
		Nanite.bEnabled = bNanite;
		MeshEditor->SetNaniteSettings(Mesh, Nanite, true);
	}

	Outcome.bNaniteEnabled = bNanite;

	if (bNanite)
	{
		// Worth saying rather than discovering in a level. Nanite does not render translucent or
		// masked materials, and a generated prop with a glass panel or a cut-out is exactly the case
		// where the mesh silently disappears.
		Outcome.Warnings.Add(
			TEXT("Nanite is on. If any material on this mesh is translucent or masked, turn it off - "
				 "Nanite will not render those."));
	}

	// ---------------------------------------------------------------------------------------------
	// LODs
	// ---------------------------------------------------------------------------------------------

	// **Beside Nanite, not instead of it.** Nanite does nothing for a translucent or masked
	// material, nothing on a platform without it, and nothing where something else wants real LODs -
	// so a prop with Nanite on and no LODs is finished for exactly one target. The two settings are
	// independent for that reason and both are honoured here.
	if (Finish.LodCount > 0)
	{
		FStaticMeshReductionOptions Reduction;

		// Let Unreal work out the swap distances. Screen sizes that are right for one prop are wrong
		// for the next, and a generated library is full of props of wildly different sizes.
		Reduction.bAutoComputeLODScreenSize = true;

		// Applied per level rather than against LOD 0, which is what anybody setting "0.5" means:
		// three levels give 50%, 25% and 12.5%, not three copies of 50%.
		float Remaining = 1.0f;

		for (int32 Level = 0; Level < Finish.LodCount; ++Level)
		{
			Remaining *= FMath::Clamp(Finish.LodReduction, 0.05f, 0.95f);

			FStaticMeshReductionSettings& Level_ = Reduction.ReductionSettings.AddDefaulted_GetRef();
			Level_.PercentTriangles = Remaining;

			// Overwritten by bAutoComputeLODScreenSize; set anyway so the array is meaningful if
			// somebody turns that off later.
			Level_.ScreenSize = FMath::Max(0.01f, 0.5f / static_cast<float>(Level + 1));
		}

		const int32 Built = MeshEditor->SetLods(Mesh, Reduction);

		if (Built <= 0)
		{
			// Not fatal. The mesh is imported and usable; it simply has one level where several were
			// asked for, and saying so beats a silent difference between what the panel claims and
			// what the asset contains.
			Outcome.Warnings.Add(FString::Printf(
				TEXT("Asked for %d LODs and none were built. Reduction can refuse a mesh with "
					 "degenerate triangles, which generated geometry often has."),
				Finish.LodCount));
		}
		else
		{
			Outcome.LodCount = Built;
		}
	}
	else
	{
		Outcome.LodCount = Mesh->GetNumLODs();
	}

	// What the *mesh* has, not what this import happened to create.
	//
	// The difference is not pedantic. Interchange reports only the assets it made, and on a
	// re-import it makes only what changed - so importing the same file twice reported a full set
	// of materials the first time and none the second, and the second run then warned that the mesh
	// had no textures while its textures sat in the Content Browser. A warning that fires on a
	// perfectly good mesh is worse than no warning at all, because it teaches people to ignore them.
	TArray<UObject*> UsedTextures;

	for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
	{
		UMaterialInterface* Material = Slot.MaterialInterface;

		if (Material == nullptr)
		{
			continue;
		}

		Outcome.Materials.Add(Material);

		TArray<UTexture*> Textures;
		// No quality level and no shader platform, so this searches all of both. The older
		// overload taking explicit enums is deprecated in 5.7 and removed in a later release.
		Material->GetUsedTextures(Textures);

		for (UTexture* Texture : Textures)
		{
			// Contains-then-Add rather than AddUnique: AddUnique returns the index either way, and
			// comparing it against the last index reports a duplicate as new whenever the duplicate
			// happens to be the most recent entry.
			if (Texture != nullptr && !UsedTextures.Contains(Texture))
			{
				UsedTextures.Add(Texture);
				Outcome.Textures.Add(Texture);
			}
		}
	}

	if (Outcome.Materials.Num() == 0)
	{
		Outcome.Warnings.Add(
			TEXT("This mesh has no material; it will render with the default one."));
	}
	else if (Outcome.Textures.Num() == 0)
	{
		Outcome.Warnings.Add(
			TEXT("This mesh's material uses no textures. Check the provider was asked for a "
				 "textured result."));
	}

	Mesh->MarkPackageDirty();
	Mesh->PostEditChange();

	// Settled before returning, so nothing downstream inspects a half-built mesh.
	FStaticMeshCompilingManager::Get().FinishCompilation({ Mesh });

	Outcome.TriangleCount = SourceTriangles;
	Outcome.VertexCount = SourceVertices;

	// Under Nanite the two diverge permanently and the difference is worth stating, because a
	// fallback an order of magnitude lighter than the mesh is what gets rendered wherever Nanite is
	// unavailable.
	if (Outcome.bNaniteEnabled)
	{
		const int32 Fallback = Mesh->GetNumTriangles(0);

		if (Fallback > 0 && Fallback < SourceTriangles)
		{
			Outcome.Warnings.Add(FString::Printf(
				TEXT("Nanite fallback is %d triangles against %d in the mesh. That fallback is what "
					 "renders where Nanite cannot."),
				Fallback, SourceTriangles));
		}
	}

	Outcome.bSuccess = true;
	return Outcome;
}

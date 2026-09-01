// glTF in, finished static mesh out. The half of the pipeline no provider can do for you.

#pragma once

#include "CoreMinimal.h"
#include "MeshForgeTypes.h"

/** Everything the importer needs. Nothing here is looked up later. */
struct FMeshImportRequest
{
	/** The .glb the provider downloaded. */
	FString AbsoluteArtifactPath;

	/**
	 * Content path to import under, e.g. "/Game/_Generated/Mesh/Meshes".
	 *
	 * Interchange creates a folder beneath this named after the source file, and puts the mesh, its
	 * materials and its textures in it. Everything for one prop therefore lands together, which is
	 * both tidier than it sounds and the only layout that survives being regenerated - see the note
	 * on Import.
	 */
	FString MeshPackagePath;

	/** Asset name for the mesh. Sanitised by the importer; a prefix is not added for you. */
	FString AssetName;

	/** Collision, lightmap UVs, Nanite, scale, origin. */
	FMeshFinishSettings Finish;

	/** Triangles above which EMeshNaniteMode::Auto turns Nanite on. */
	int32 NaniteTriangleThreshold = 50000;

	/** Save every package this creates. Right for headless runs, wrong for an interactive batch. */
	bool bSaveAssets = false;
};

/**
 * Imports a generated glTF and finishes it into something that belongs in a level.
 *
 * Two halves, and the split matters.
 *
 * **Translation** is Interchange's, not ours.** A .glb carries a mesh, its materials and its
 * textures, and Unreal already knows how to read all three. Writing a glTF parser here would mean
 * owning a format's edge cases forever in exchange for nothing.
 *
 * **Finishing is ours, and it is the whole point.** What comes out of a generator is a surface
 * extracted from a voxel field: correct, dense, centred on nothing in particular, sized to a unit
 * box, with one UV set and no collision. Every one of those is wrong for a game asset, none of them
 * is a defect in the generator, and fixing them is not something a provider can do because none of
 * them are properties of the model - they are properties of the engine it is going into.
 */
class MESHFORGE_API FMeshImporter
{
public:

	/**
	 * Import and finish. Blocking; call on the game thread.
	 *
	 * Never returns a partial failure silently: anything that succeeded imperfectly - no textures, a
	 * refused lightmap unwrap, collision that could not be fitted - comes back in Warnings with the
	 * mesh still valid.
	 */
	static FMeshImportOutcome Import(const FMeshImportRequest& Request);

	/**
	 * Re-finish a mesh already in the project, without importing anything.
	 *
	 * The cheap loop. Changing a collision mode or a scale costs no generation, no download and no
	 * money, so it deserves a path that does not pretend otherwise.
	 */
	static FMeshImportOutcome Refinish(
		UStaticMesh* Mesh,
		const FMeshFinishSettings& Finish,
		int32 NaniteTriangleThreshold);
};

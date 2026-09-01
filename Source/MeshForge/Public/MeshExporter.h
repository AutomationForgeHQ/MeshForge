// Static mesh out to glTF. The other direction from MeshImporter, and for the same reason.

#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

/** How a mesh is written out. */
struct MESHFORGE_API FMeshExportOptions
{
	/**
	 * Write the material textures into the file. Off by default, deliberately.
	 *
	 * The reason this exporter exists is to hand a mesh to a service that is about to *replace* its
	 * textures, and a service cannot use what it is going to throw away. Including them costs the
	 * bake, costs the megabytes, and on a vendor that takes the model as a base64 data URI those
	 * megabytes are the whole difference between a request that goes through and one that does not.
	 *
	 * Turn it on when the file is for a person rather than for a texturing service.
	 */
	bool bIncludeTextures = false;

	/** Which LOD to write. Zero is the full-detail mesh. */
	int32 LevelOfDetail = 0;

	/**
	 * Centimetres to glTF's metres.
	 *
	 * The engine default, and the one Interchange undoes on the way back in - so a mesh exported and
	 * re-imported through this plugin comes back the size it left. Changing it means a retextured
	 * corridor returns a hundred times too big, which looks like a failure of the vendor.
	 */
	float UniformScale = 0.01f;
};

/** What an export produced, or why it produced nothing. */
struct MESHFORGE_API FMeshExportResult
{
	bool bSuccess = false;

	/** Absolute path to the file written. Empty on failure. */
	FString Path;

	int64 SizeBytes = 0;

	/** A sentence for a person. Empty on success. */
	FString Error;

	/** What the exporter grumbled about but carried on through. */
	TArray<FString> Warnings;
};

/**
 * Writing a project mesh out as glTF.
 *
 * **This is what makes the post stage work on meshes MeshForge did not generate.** A retexture
 * service can be pointed at its own finished task for nothing, which covers regenerating our own
 * takes - and covers none of the case that actually matters, which is a modular corridor somebody
 * modelled by hand. That mesh has never left the project, so the only way to hand it to anybody is
 * to write it out.
 *
 * A thin wrapper over Epic's glTF exporter rather than a mesh writer of our own: the engine already
 * ships one, it handles LODs, materials, vertex colours and the coordinate conversion, and a second
 * implementation would drift from it silently.
 *
 * **Game thread only.** It reads render data and can bake materials.
 */
class MESHFORGE_API FMeshExporter
{
public:

	/** Write one static mesh to an absolute .glb path, creating the directory if needed. */
	static FMeshExportResult ToGlb(
		UStaticMesh* Mesh,
		const FString& AbsolutePath,
		const FMeshExportOptions& Options = FMeshExportOptions());

	/**
	 * The same, then read the file back into memory.
	 *
	 * The file is kept rather than deleted. It costs nothing next to what it took to make, and a
	 * file on disk is the difference between a failed upload somebody can inspect and one they can
	 * only re-run.
	 */
	static FMeshExportResult ToGlbBytes(
		UStaticMesh* Mesh,
		const FString& AbsolutePath,
		TArray<uint8>& OutBytes,
		const FMeshExportOptions& Options = FMeshExportOptions());
};

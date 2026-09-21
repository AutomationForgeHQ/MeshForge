// Putting positions worked out somewhere else back onto a mesh of our own, vertex for vertex.

#pragma once

#include "CoreMinimal.h"

/**
 * A mesh sent out, changed by something else, and matched back to itself by where its vertices stood.
 *
 * **Why not import what comes back.** glTF splits vertices along UV and normal seams and every importer
 * builds a new mesh from them - 7,317 vertices go out and 7,322 come back - and a tool on the other side
 * may merge them again. Either way the order is lost, and with it every attribute keyed to it: UVs,
 * material slots, skin weights, morph targets. So the thing on the other side reports each vertex where
 * it arrived and where it left it, in one order, and each of our vertices takes the answer of whichever
 * arrivals stood where it stands. Vertices that were merged over there move together over here, which is
 * what keeps a seam closed.
 *
 * The file is written by MeshForge's Blender add-on and by MeshForge Garment's fitting runner: "GFP1", a
 * uint32 count, then the before and after positions as float32 triples in Blender's space and metres.
 */
class MESHFORGE_API FMeshPositionMap
{
public:

	/** Read a positions file, converting Blender's space and metres to Unreal's (x, -y, z) x 100. */
	static bool ReadFile(const FString& Path, TArray<FVector3f>& OutBefore, TArray<FVector3f>& OutAfter);

	/**
	 * Each position of Snapshot takes the mean of the After positions whose Before stood there, within a
	 * hundredth of a centimetre. False with the reason when any of them is nowhere in Before.
	 */
	static bool Apply(const TArray<FVector3f>& Snapshot, const TArray<FVector3f>& Before, const TArray<FVector3f>& After,
		TArray<FVector3f>& OutPositions, FString& OutError);
};

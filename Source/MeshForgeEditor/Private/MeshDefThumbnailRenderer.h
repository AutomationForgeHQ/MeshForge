// What a Mesh Definition looks like in the Content Browser: what it made, or what it was made from.

#pragma once

#include "CoreMinimal.h"
#include "ThumbnailRendering/ThumbnailRenderer.h"
#include "MeshDefThumbnailRenderer.generated.h"

class UMeshDef;

/**
 * Draws a Mesh Definition's thumbnail as the thing it is about.
 *
 * Every definition used to show the same class icon, so a folder of thirty was a folder of thirty
 * identical tiles read by name. The tile now shows, in order: the newest mesh the definition produced
 * (a skinned garment, a post-processed mesh, the imported take), else the picture chosen as its main
 * image, else nothing - and the class icon stands in.
 *
 * **It draws nothing itself.** The mesh or the picture is handed to the renderer Unreal already has
 * for that class, so a definition's tile looks exactly like its mesh's tile beside it.
 */
UCLASS()
class UMeshDefThumbnailRenderer : public UThumbnailRenderer
{
	GENERATED_BODY()

public:

	virtual bool CanVisualizeAsset(UObject* Object) override;
	virtual void Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height,
		FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily) override;

	/** What a definition's tile shows, or null for the class icon. Loads it if needed. */
	static UObject* ResolveSubject(const UMeshDef* Def);

	/** Bound to UPackage::PackageMarkedDirtyEvent: a definition that changed gets its cached thumbnail redrawn at save. */
	static void MarkThumbnailStale(UPackage* Package, bool bWasDirty);
};

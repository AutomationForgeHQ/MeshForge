// Turning generated pictures into project assets, losslessly.

#pragma once

#include "CoreMinimal.h"

class UMeshDef;
class UTexture2D;

/**
 * Writes PNG bytes into the project as a real `UTexture2D`.
 *
 * **Every generated picture becomes an asset, and that is the point.** A viewer full of images that
 * are not assets cannot be dragged into a material, cannot be found again through the Content
 * Browser, and quietly teaches people that generated work lives somewhere outside the project. The
 * concept image was drawn and thrown away before this existed - used as bytes for one submission
 * and never seen again.
 *
 * **Nothing is lost to compression.** The bytes go into `FTextureSource`, which Unreal keeps inside
 * the asset alongside the compressed version it renders. Sending a picture back to a provider for a
 * second pass reads that source rather than the block-compressed copy, so a round trip through
 * Unreal costs no quality - which is what makes the Images tab safe to treat as the only copy.
 */
class MESHFORGE_API FMeshImageIngest
{
public:

	/**
	 * Create a texture asset from PNG (or JPEG/WebP) bytes.
	 *
	 * `Folder` is a content path such as `/Game/_Generated/Mesh/Images/MyProp`. `BaseName` is
	 * uniquified rather than overwritten, because two runs of the same stage are two pictures and
	 * silently replacing the first would lose a candidate somebody may still want - on a provider
	 * with no seed, a picture discarded cannot be drawn again.
	 *
	 * Returns null on failure, with the reason in OutError.
	 */
	static UTexture2D* CreateTextureAsset(
		const TArray<uint8>& EncodedBytes,
		const FString& Folder,
		const FString& BaseName,
		FString& OutError);

	/**
	 * Read a texture back out as PNG bytes, ready to hand to a provider.
	 *
	 * Reads `FTextureSource` - the original pixels - not the block-compressed copy the renderer
	 * uses, which is what makes a round trip through the project cost nothing. A texture with no
	 * source data (cooked, or built at runtime) fails here with a sentence saying so, rather than
	 * quietly sending a DXT-mangled version of somebody's reference to a paid API.
	 */
	static bool EncodePng(const UTexture2D* Texture, TArray<uint8>& OutPng, FString& OutError);

	/**
	 * Where a definition's generated pictures belong.
	 *
	 * A folder per definition, beside the meshes it produces, so deleting one prop's work is one
	 * folder rather than a hunt through a flat pile - and so a definition that has produced twenty
	 * candidates has not made the Content Browser unusable for everybody else.
	 */
	static FString ImageFolderFor(const UMeshDef* Def);
};

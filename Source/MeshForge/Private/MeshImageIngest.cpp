#include "MeshImageIngest.h"

#include "MeshDef.h"
#include "MeshForge.h"
#include "MeshForgeSettings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "ImageCore.h"
#include "ImageUtils.h"

FString FMeshImageIngest::ImageFolderFor(const UMeshDef* Def)
{
	// Beside the definition, under its own generated folder, rather than in a shared Images root -
	// so everything one definition made lives together and can be collapsed together.
	const FString Name = Def ? Def->GetName() : TEXT("Unnamed");
	return UMeshForgeSettings::Get()->GetImagesFolder(Name);
}

UTexture2D* FMeshImageIngest::CreateTextureAsset(
	const TArray<uint8>& EncodedBytes,
	const FString& Folder,
	const FString& BaseName,
	FString& OutError)
{
	OutError.Reset();

	if (EncodedBytes.Num() == 0)
	{
		OutError = TEXT("No image data.");
		return nullptr;
	}

	// The format is detected rather than assumed. Providers are not consistent about it - the same
	// vendor returns PNG from one endpoint and JPEG from another - and guessing produces a texture
	// of noise rather than an error.
	IImageWrapperModule& WrapperModule =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	const EImageFormat Format = WrapperModule.DetectImageFormat(EncodedBytes.GetData(), EncodedBytes.Num());
	if (Format == EImageFormat::Invalid)
	{
		OutError = TEXT("Those bytes are not an image this build can read.");
		return nullptr;
	}

	const TSharedPtr<IImageWrapper> Wrapper = WrapperModule.CreateImageWrapper(Format);
	if (!Wrapper.IsValid() || !Wrapper->SetCompressed(EncodedBytes.GetData(), EncodedBytes.Num()))
	{
		OutError = TEXT("The image could not be decoded.");
		return nullptr;
	}

	TArray64<uint8> Raw;
	if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
	{
		OutError = TEXT("The image decoded to no pixels.");
		return nullptr;
	}

	const int32 Width  = Wrapper->GetWidth();
	const int32 Height = Wrapper->GetHeight();

	if (Width <= 0 || Height <= 0)
	{
		OutError = TEXT("The image has no size.");
		return nullptr;
	}

	// Uniquified rather than overwritten: two runs of a stage are two pictures, and replacing the
	// first silently loses a candidate that on a seedless provider can never be drawn again.
	FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

	FString PackageName;
	FString AssetName;
	AssetTools.Get().CreateUniqueAssetName(Folder / BaseName, FString(), PackageName, AssetName);

	UPackage* Package = CreatePackage(*PackageName);
	if (Package == nullptr)
	{
		OutError = FString::Printf(TEXT("Could not create a package at '%s'."), *PackageName);
		return nullptr;
	}

	UTexture2D* Texture = NewObject<UTexture2D>(
		Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);

	if (Texture == nullptr)
	{
		OutError = TEXT("Could not create the texture object.");
		return nullptr;
	}

	// Init the *source*, not the platform data. This is what makes the original pixels survive
	// inside the asset: Unreal keeps FTextureSource beside the compressed version it renders, so
	// sending this picture back to a provider later reads these bytes rather than a block-compressed
	// approximation of them.
	Texture->Source.Init(Width, Height, /*NumSlices*/ 1, /*NumMips*/ 1, TSF_BGRA8, Raw.GetData());

	// sRGB because these are colour images a person looks at. A normal or roughness map arriving
	// through this path would want linear, which is a decision for whatever asks for one.
	Texture->SRGB = true;
	Texture->CompressionSettings = TC_Default;
	Texture->MipGenSettings = TMGS_FromTextureGroup;
	Texture->LODGroup = TEXTUREGROUP_World;

	Texture->UpdateResource();
	Texture->PostEditChange();
	Texture->MarkPackageDirty();

	FAssetRegistryModule::AssetCreated(Texture);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;

	const FString FileName = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());

	if (!UPackage::SavePackage(Package, Texture, *FileName, SaveArgs))
	{
		// Not fatal. The texture exists in memory and the definition can point at it; it simply has
		// not reached disk, which is a thing somebody can fix by pressing Save. Failing the whole
		// stage over it would discard a picture that was paid for.
		UE_LOG(LogMeshForge, Warning,
			TEXT("Created '%s' but could not write it to disk. It is unsaved rather than lost."),
			*PackageName);
	}

	UE_LOG(LogMeshForge, Log, TEXT("Ingested %dx%d image as '%s'."), Width, Height, *PackageName);
	return Texture;
}

bool FMeshImageIngest::EncodePng(const UTexture2D* Texture, TArray<uint8>& OutPng, FString& OutError)
{
	if (Texture == nullptr)
	{
		OutError = TEXT("No texture.");
		return false;
	}

#if WITH_EDITORONLY_DATA
	FTextureSource& Source = const_cast<UTexture2D*>(Texture)->Source;

	if (!Source.IsValid())
	{
		OutError = FString::Printf(
			TEXT("'%s' has no source data. A texture built without it - one cooked, or created at "
				 "runtime - cannot be sent to a generator."),
			*Texture->GetName());
		return false;
	}

	FImage Image;
	if (!Source.GetMipImage(Image, 0))
	{
		OutError = FString::Printf(
			TEXT("Could not read the source pixels of '%s'."), *Texture->GetName());
		return false;
	}

	// Normalised to 8-bit BGRA in sRGB, which is what every provider expects and what PNG encodes
	// without a second conversion. Via FImage rather than by reading raw mip bytes, because FImage
	// carries its own format and gamma: doing it by hand meant enumerating the source formats we
	// happened to support and rejecting a 16-bit or float texture that was perfectly usable.
	Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);

	TArray64<uint8> Encoded;
	if (!FImageUtils::CompressImage(Encoded, TEXT("png"), Image, 0))
	{
		OutError = FString::Printf(TEXT("Could not encode '%s' as PNG."), *Texture->GetName());
		return false;
	}

	// Providers take a TArray, and an image large enough to overflow one would have to be about two
	// gigabytes - but checking is cheaper than the truncation would be.
	if (!IntFitsIn<int32>(Encoded.Num()))
	{
		OutError = FString::Printf(TEXT("'%s' encodes to more than 2GB."), *Texture->GetName());
		return false;
	}

	OutPng.Append(Encoded.GetData(), static_cast<int32>(Encoded.Num()));
	return OutPng.Num() > 0;
#else
	OutError = TEXT("Textures can only be read in an editor build.");
	return false;
#endif
}

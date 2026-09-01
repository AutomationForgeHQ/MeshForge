#include "MeshForgeSettings.h"

#include "MeshForge.h"
#include "Misc/Paths.h"

UMeshForgeSettings::UMeshForgeSettings()
{
	// Defaults that suit a prop, which is what this plugin is for. A character generated as a static
	// mesh wants most of the same answers; a landscape does not, and is out of scope.
	DefaultControl.Quality = EMeshQuality::Standard;
	DefaultControl.bRemesh = true;
	DefaultControl.bRemoveBackground = true;

	DefaultFinish.Collision = EMeshCollisionMode::ConvexDecomposition;
	DefaultFinish.bGenerateLightmapUVs = true;
	DefaultFinish.Nanite = EMeshNaniteMode::Auto;
	// Zero, so a generated prop keeps the size glTF gave it - a unit box, which Interchange
	// imports as 100cm. That is a reasonable default object; anything that needs a real size
	// says so per definition.
	DefaultFinish.TargetSizeCm = 0.f;
	DefaultFinish.bOriginAtBase = true;
}

const UMeshForgeSettings* UMeshForgeSettings::Get()
{
	return GetDefault<UMeshForgeSettings>();
}

TArray<FString> UMeshForgeSettings::GetProviderOptions()
{
	// First, so a field can always be put back. A GetOptions dropdown offers only what this returns,
	// and without an explicit entry choosing a provider once would be irreversible - "None" parses
	// straight to NAME_None, which is exactly what an unset field holds.
	TArray<FString> Options{ TEXT("None") };

	// Asked of the registry every time the dropdown opens, so enabling a provider plugin makes it
	// appear without anything here being told.
	if (const FMeshForgeModule* Module = FMeshForgeModule::GetPtrIfLoaded())
	{
		TArray<FString> Registered;
		for (const FName Id : Module->GetProviderIds())
		{
			Registered.Add(Id.ToString());
		}

		// Sorted among themselves, with None kept at the top rather than sorted into the middle.
		Registered.Sort();
		Options.Append(Registered);
	}

	return Options;
}

FString UMeshForgeSettings::GetStagingDirectory() const
{
	if (!StagingDirectory.IsEmpty())
	{
		return StagingDirectory;
	}

	return FPaths::ConvertRelativePathToFull(
		FPaths::ProjectIntermediateDir() / TEXT("MeshForge"));
}

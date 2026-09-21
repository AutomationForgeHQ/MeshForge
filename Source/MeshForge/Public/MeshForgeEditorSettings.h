// What is mine rather than the team's: which provider I use.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MeshForgeEditorSettings.generated.h"

/**
 * Per-person MeshForge preferences.
 *
 * Tier two of the family's three. `EditorPerProjectUserSettings` rather than `defaultconfig`, so
 * nothing here is committed: which provider I happen to use, and nothing else.
 *
 * **No keys, deliberately.** MeshForge is a core and spends nothing; a key belongs to the plugin
 * that spends it, on that plugin's own page - MeshForge Cloud for Meshy and Tripo, MeshForge
 * TRELLIS.2 for its runner and Hugging Face. This page used to carry a generic "name a service,
 * then paste a key" box, which worked and read as though the core needed a key of its own.
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "MeshForge"))
class MESHFORGE_API UMeshForgeEditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:

	virtual FName GetCategoryName() const override { return TEXT("Automation Forge"); }

	static UMeshForgeEditorSettings* Get();

	/**
	 * Provider this machine uses, overriding the project's default.
	 *
	 * The case this exists for: a studio's project names a hosted provider so that everyone gets the
	 * same result, and one person with a good GPU runs the local one instead. Neither answer is
	 * wrong and neither belongs in the other's file.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Providers",
		meta = (GetOptions = "/Script/MeshForge.MeshForgeSettings.GetProviderOptions"))
	FName ProviderOverride;

	/**
	 * Blender, for the "Edit in Blender" post step. Empty looks in the usual places.
	 *
	 * On Windows that is every `Blender Foundation\Blender *` under Program Files, newest first;
	 * elsewhere, `blender` on the PATH. Mine rather than the team's, because where Blender is installed
	 * is a fact about this machine.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Blender",
		meta = (FilePathFilter = "Blender executable (blender.exe)|blender.exe|All files (*.*)|*.*"))
	FFilePath BlenderExecutable;

	/** The Blender this machine would launch, after applying the rule above. Empty when none is found. */
	FString ResolveBlender() const;

	/**
	 * Show the floor in a definition's Mesh tab. Off by default.
	 *
	 * Most generated meshes keep their pivot at their centre, so the floor cuts through the middle of
	 * them. Turn it on for a prop that has been finished with its origin at its base. Set from the tab's
	 * own Floor checkbox, and remembered here.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Preview")
	bool bShowPreviewFloor = false;

};

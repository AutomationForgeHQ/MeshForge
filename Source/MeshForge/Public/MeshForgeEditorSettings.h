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

};

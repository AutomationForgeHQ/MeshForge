// What the team shares: where meshes land, which providers are allowed, how they are finished.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MeshForgeTypes.h"
#include "MeshForgeSettings.generated.h"

/**
 * Project settings for MeshForge.
 *
 * Tier one of the family's three: committed, and shared by everyone working on the project. Output
 * paths, naming, quality defaults and which providers a project allows belong here. Personal
 * choices - which provider *I* use, my local runner address - belong in the editor preferences
 * beside this, and secrets belong in neither.
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "MeshForge"))
class MESHFORGE_API UMeshForgeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:

	UMeshForgeSettings();

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	static const UMeshForgeSettings* Get();

	// ---------------------------------------------------------------------------------------------
	// Routing
	// ---------------------------------------------------------------------------------------------

	/**
	 * Provider a new definition is created with.
	 *
	 * Offered from the live registry, so enabling a provider plugin makes it selectable with nothing
	 * here being told. "None" is always first, so choosing one is never irreversible.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Providers",
		meta = (GetOptions = "GetProviderOptions"))
	FName DefaultProviderId;

	/** Model a new definition is created with, where the provider has more than one. */
	UPROPERTY(config, EditAnywhere, Category = "Providers")
	FString DefaultModelId;

	UFUNCTION()
	static TArray<FString> GetProviderOptions();

	// ---------------------------------------------------------------------------------------------
	// Output
	// ---------------------------------------------------------------------------------------------

	/**
	 * Where generated content goes.
	 *
	 * One root, subdivided by kind below rather than by date or by run. A generated library that all
	 * lands in one folder becomes unusable at about thirty assets, and the moment to decide the
	 * layout is before the first one is made rather than after the hundredth.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Output", meta = (ContentDir))
	FString OutputContentPath = TEXT("/Game/_Generated/Mesh");

	/** Where mesh definitions are created. */
	FString GetDefinitionsPath() const { return OutputContentPath / TEXT("Definitions"); }

	/** Where imported static meshes land. Superseded by the per-take folders below. */
	FString GetMeshesPath() const { return OutputContentPath / TEXT("Meshes"); }

	// ---------------------------------------------------------------------------------------------
	// One folder per definition, one folder per take
	//
	// **This replaced a layout where every take in the project imported into the same folder**, and
	// the reason is worth keeping. Artifacts are filed in the vault under a fixed name, so Interchange
	// - which names its destination after the source file - put every mesh ever generated into
	// `Meshes/artifact`. Two takes of the same definition then shared a material asset, and importing
	// the second created its textures without repointing that material at them: the mesh in the
	// project kept rendering the *previous* take's textures, silently, with the new ones sitting
	// beside them. A retexture appeared to do nothing.
	//
	// Giving each take its own folder makes that impossible rather than unlikely. It costs asset
	// count, which is the right thing to spend: takes are meant to accumulate, and collapsing down to
	// the chosen one is a deliberate act with the library behind it.
	// ---------------------------------------------------------------------------------------------

	/** A definition's own generated content, beside the definition asset itself. */
	FString GetGeneratedFolder(const FString& DefinitionName) const
	{
		return GetDefinitionsPath() / (DefinitionName + TEXT("_Generated"));
	}

	/** Pictures drawn or added for this definition. */
	FString GetImagesFolder(const FString& DefinitionName) const
	{
		return GetGeneratedFolder(DefinitionName) / TEXT("Images");
	}

	/** Pictures the reference stage made from other pictures. */
	FString GetReferencesFolder(const FString& DefinitionName) const
	{
		return GetGeneratedFolder(DefinitionName) / TEXT("References");
	}

	/** One generated take: its mesh, materials and textures, and nothing else's. */
	FString GetMeshTakeFolder(const FString& DefinitionName, const FString& TakeId) const
	{
		return GetGeneratedFolder(DefinitionName) / TEXT("Mesh") / TakeId;
	}

	/** One post-processed take. Chaining steps means each one lands in its own folder too. */
	FString GetPostTakeFolder(const FString& DefinitionName, const FString& TakeId) const
	{
		return GetGeneratedFolder(DefinitionName) / TEXT("Post") / TakeId;
	}

	/**
	 * Materials and textures get no folders of their own, and that is a decision rather than an
	 * oversight.
	 *
	 * Interchange puts everything a glTF contains into a folder named after the source file, beneath
	 * the meshes path - so one prop is one self-contained folder, and deleting a prop is deleting a
	 * folder. Moving the supporting assets out into shared Materials and Textures folders reads
	 * better in the Content Browser and breaks the thing that matters more: **re-import.**
	 *
	 * Interchange replaces an asset where it left it. Move the material away and the next generation
	 * of the same definition cannot find it, so it makes a second one - and the move then fails on
	 * the name collision. That is not hypothetical; it is what happened when this was tried.
	 * Generated content is regenerated constantly, and a layout that only works the first time is
	 * the wrong layout.
	 */

	/** Where concept images drawn from a prompt are kept, when a provider draws one. */
	FString GetConceptsPath() const { return OutputContentPath / TEXT("Concepts"); }

	/**
	 * Absolute path on disk where downloaded artifacts are staged before import.
	 *
	 * Outside the content folder deliberately: a GLB is an intermediate, not an asset, and one that
	 * lands in Content gets committed by somebody eventually. Empty means the project's Intermediate
	 * folder, which is what almost everyone should use.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Output")
	FString StagingDirectory;

	/** Resolved staging directory, with the empty default filled in. */
	FString GetStagingDirectory() const;

	// ---------------------------------------------------------------------------------------------
	// Defaults
	// ---------------------------------------------------------------------------------------------

	/** How a new definition is set up to generate. */
	UPROPERTY(config, EditAnywhere, Category = "Defaults")
	FMeshControl DefaultControl;

	/** How a new definition is set up to be finished on import. */
	UPROPERTY(config, EditAnywhere, Category = "Defaults")
	FMeshFinishSettings DefaultFinish;

	/**
	 * Triangles above which EMeshNaniteMode::Auto turns Nanite on.
	 *
	 * A generated prop varies by two orders of magnitude between Draft and Ultra, so a fixed answer
	 * is wrong for one of them. This is the line between the two.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Defaults", meta = (ClampMin = 1000))
	int32 NaniteTriangleThreshold = 50000;

	// ---------------------------------------------------------------------------------------------
	// Behaviour
	// ---------------------------------------------------------------------------------------------

	/**
	 * Seconds between polls of a running job.
	 *
	 * Local generation finishes in seconds, so a slow poll is most of the perceived wait; hosted
	 * providers rate-limit, so a fast one is rude. Two is the compromise, and it is a setting because
	 * a project generating a hundred props at once wants a different answer.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Behaviour", meta = (ClampMin = 1, ClampMax = 60))
	int32 PollIntervalSeconds = 2;

	/** Give up on a job after this long. Zero waits forever, which is never what anyone wants. */
	UPROPERTY(config, EditAnywhere, Category = "Behaviour", meta = (ClampMin = 0))
	int32 JobTimeoutSeconds = 900;

	/**
	 * Import the first successful take without waiting to be asked.
	 *
	 * True is right for a local provider, where generating again costs nothing and the fast loop is
	 * the point. On a metered provider it silently spends the download, which is why it is a setting
	 * rather than the behaviour.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Behaviour")
	bool bAutoImportFirstSuccess = true;

	/**
	 * Save every asset the pipeline creates, as it creates it.
	 *
	 * Off by default: an unsaved asset is visible, usable and reviewable, and forcing a save on a
	 * batch of forty generated props writes forty packages somebody may not want. Turn it on for
	 * headless and CI runs, where nothing is going to press Ctrl+S.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Behaviour")
	bool bSaveGeneratedAssets = false;
};

// How a plugin that does not depend on MeshForge adds a post step to it anyway.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "Templates/Function.h"

class UClass;
class UObject;

/**
 * Everything a registered post step is handed when it runs.
 *
 * **Plain data, deliberately.** This header is read by plugins that never link MeshForge - a
 * Narrative Pro add-on that wants to turn a finished garment into an inventory item, say - so it
 * may contain nothing that needs a MeshForge symbol. No UMeshDef, no pipeline class, nothing with
 * an export macro. A step that needed the definition would start making the definition's decisions.
 */
struct FMeshForgePostStepContext
{
	/** The asset this step works on: the previous step's output, or whatever the step was pointed at. */
	UObject* Input = nullptr;

	/**
	 * What Input was itself made from, where the chain recorded it: for a skinned garment, the wrapped
	 * static mesh it was skinned from. Null when Input was chosen by hand or has no recorded source.
	 */
	UObject* InputMadeFrom = nullptr;

	/** This step's settings - an instance of the registered SettingsClass, owned by the step. */
	UObject* Settings = nullptr;

	/** The definition's asset name and full object path, for naming and for the log. */
	FString DefinitionName;
	FString DefinitionPath;

	/** What the object is, as the definition describes it. */
	FString Prompt;

	/**
	 * The long package path this step's output belongs in, and a name to start from.
	 *
	 * Offered rather than imposed. The default keeps every take of a definition in that definition's
	 * own folder, which is what makes deleting a prop deleting a folder; a step whose output belongs
	 * somewhere a game expects to find it may write there instead and say so in its summary.
	 */
	FString OutputPackagePath;
	FString SuggestedAssetName;
};

/** What a registered post step produced, or why it produced nothing. */
struct FMeshForgePostStepOutcome
{
	/** The asset made. Null together with an empty Error is treated as a failure, never as success. */
	UObject* Asset = nullptr;

	/** One line for the ledger: what this step actually did. */
	FString Summary;

	/** A sentence for a person. Empty on success. */
	FString Error;
};

/**
 * One kind of post step a plugin offers.
 *
 * Registered steps run on the game thread, on assets, after the steps before them have imported:
 * the same footing as MeshForgeGarment's skinning step. They are for work that creates or edits
 * project assets - an inventory item, a physics asset, a data table row - not for work on geometry
 * bytes, which a MeshForge add-on does by deriving a pipeline class.
 */
struct FMeshForgePostStepType
{
	/**
	 * Stable and saved into every definition that uses the step. Never rename one that has shipped.
	 *
	 * Prefix it with the owning plugin - `NP_Clothing.ClothingItem` - so two plugins cannot collide.
	 */
	FName Id;

	/** What the step is called in the picker and in the chain. */
	FText DisplayName;

	/** What it does and what it needs, in a sentence or two. Shown as the picker's tooltip. */
	FText Description;

	/** The plugin that registered it, so a definition opened without it can say what to install. */
	FString OwningPlugin;

	/**
	 * The settings object's class. An EditInlineNew UObject subclass from the registering module.
	 *
	 * A real class rather than a bag of strings for the reason every pipeline gives: the details
	 * panel draws it, agents set it through the object property tools, and its reflected properties
	 * feed the stage hash so editing a setting marks the output stale.
	 */
	UClass* SettingsClass = nullptr;

	/** Which input asset classes the step accepts. Matched with IsChildOf. Empty accepts anything. */
	TArray<UClass*> InputClasses;

	/** Why these settings cannot run, or empty when they can. Optional. Game thread. */
	TFunction<FString(const UObject* Settings)> Validate;

	/** Do the work. Game thread; may create, modify and save assets. Required. */
	TFunction<void(const FMeshForgePostStepContext& Context, FMeshForgePostStepOutcome& Outcome)> Run;
};

/**
 * MeshForge's extension surface, reachable from a plugin that neither links MeshForge nor names it
 * in its .uplugin.
 *
 * **The pattern is the family's, and the build half matters as much as the runtime half.** The
 * caller adds MeshForge to `PrivateIncludePathModuleNames` only when a `MeshForge/MeshForge.uplugin`
 * is found beside it, and defines `WITH_MESHFORGE` to match; the `#include` and the registration sit
 * behind that define. Without the build-time check, UBT refuses to compile the caller at all for
 * somebody who installed it alone - which is the one situation this interface exists for.
 *
 * Everything here is inline or pure virtual, so no symbol is ever imported from MeshForge.
 */
class IMeshForgeExtensionsModule : public IModuleInterface
{
public:

	/**
	 * MeshForge, loading it if it is installed but has not started yet. Null when it is not
	 * installed or not enabled, which is the ordinary case and needs no warning.
	 *
	 * Loading rather than looking up, because two plugins in the same loading phase start in an
	 * order nobody controls. MeshForge's modules load at PostEngineInit; register from a module that
	 * loads there too.
	 */
	static IMeshForgeExtensionsModule* GetOrLoad()
	{
		if (!FModuleManager::Get().ModuleExists(TEXT("MeshForge")))
		{
			return nullptr;
		}
		return FModuleManager::Get().LoadModulePtr<IMeshForgeExtensionsModule>(TEXT("MeshForge"));
	}

	/** Null unless MeshForge is already up. For shutdown paths, which must never load anything. */
	static IMeshForgeExtensionsModule* GetIfLoaded()
	{
		return FModuleManager::GetModulePtr<IMeshForgeExtensionsModule>(TEXT("MeshForge"));
	}

	/** Offer a post step. Registering an id again replaces the earlier one, so hot reload survives. */
	virtual void RegisterPostStepType(const FMeshForgePostStepType& Type) = 0;

	/** Withdraw it again. Call from ShutdownModule: the closures point into the caller's module. */
	virtual void UnregisterPostStepType(FName Id) = 0;
};

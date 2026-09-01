#pragma once

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

class IMeshProvider;

/** Filter the Output Log on "LogMeshForge" to follow submit, poll, download, import and finish. */
MESHFORGE_API DECLARE_LOG_CATEGORY_EXTERN(LogMeshForge, Log, All);

/**
 * MeshForge's module, and the registry add-on plugins join.
 *
 * Providers live here rather than on the subsystem for one reason: module startup order. An add-on
 * loads and registers whenever its own loading phase says, which may be before or after the editor
 * builds the subsystem. Holding the list on the module means neither order loses a provider - the
 * subsystem reads whatever is registered when it comes up, and hears about anything that arrives
 * later through OnProvidersChanged.
 *
 * A shared pointer rather than an IModularFeature: the pipeline hands providers into callbacks that
 * outlive the call, and a raw pointer whose owning module unloaded mid-batch is a crash rather than
 * an error message.
 *
 * **MeshForge ships no provider of its own.** Unlike MotionForge, which carries Uthana, there is no
 * first-party mesh service to register - every provider is an add-on. That makes the empty registry
 * a normal state rather than a fault, and the editor says so plainly instead of reporting an error.
 */
class MESHFORGE_API FMeshForgeModule : public IModuleInterface
{
public:

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * MeshForge's module, loading it if it has not started yet.
	 *
	 * **Use this from an add-on's StartupModule.** Two plugins in the same loading phase start in an
	 * order nobody controls, and a `.uplugin` dependency guarantees only that MeshForge is *enabled*
	 * - not that its module ran first. Merely looking the module up therefore works on some runs and
	 * returns null on others, and the failure is a provider that silently never registers. Loading
	 * on demand removes the ordering question entirely.
	 */
	static FMeshForgeModule* GetPtr();

	/**
	 * The module if it is already up, never loading it.
	 *
	 * For shutdown paths, where loading a module in order to tell it something is being torn down
	 * would be worse than doing nothing.
	 */
	static FMeshForgeModule* GetPtrIfLoaded();

	/**
	 * Make a provider available to the pipeline.
	 *
	 * Call from an add-on module's StartupModule. Registering the same id twice replaces the first,
	 * which makes hot reload survivable.
	 */
	void RegisterProvider(TSharedRef<IMeshProvider> Provider);

	/** Take a provider back out again. Call from ShutdownModule, or in-flight jobs will outlive it. */
	void UnregisterProvider(FName ProviderId);

	TSharedPtr<IMeshProvider> FindProvider(FName ProviderId) const;
	TArray<FName> GetProviderIds() const;

	DECLARE_MULTICAST_DELEGATE(FOnProvidersChanged);
	FOnProvidersChanged OnProvidersChanged;

private:

	TMap<FName, TSharedPtr<IMeshProvider>> Providers;
};

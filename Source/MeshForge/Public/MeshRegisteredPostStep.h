// A post step whose work lives in another plugin that registered it, rather than in a class of its own.

#pragma once

#include "CoreMinimal.h"
#include "MeshPostPipeline.h"
#include "MeshRegisteredPostStep.generated.h"

struct FMeshForgePostStepType;

/**
 * The chain's handle on a step registered through IMeshForgeExtensionsModule.
 *
 * **One class for every registered step, and that is what lets the registering plugin skip linking
 * MeshForge.** It cannot derive a pipeline class without MeshForge's symbols, so it registers a
 * settings class and a function instead, and this object carries both on the definition: the id it
 * was made for, and an instance of the settings class drawn inline in the panel.
 *
 * **A definition opened without that plugin stays intact and says so.** The settings object's class
 * is gone and loads as null; the id is kept, so the step names the plugin to install and refuses to
 * run rather than disappearing from the chain.
 */
UCLASS(BlueprintType, EditInlineNew, meta = (DisplayName = "Registered post step"))
class MESHFORGE_API UMeshRegisteredPostStep : public UMeshPostPipeline
{
	GENERATED_BODY()

public:

	/** Which registered step this is. Set when the step is added; saved with the definition. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Step")
	FName StepType;

	/** The plugin that registered it, recorded so a definition opened without it can name it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Step")
	FString OwningPlugin;

	/** The step's own settings, an instance of the class its plugin registered. */
	UPROPERTY(EditAnywhere, Instanced, BlueprintReadOnly, Category = "Settings", meta = (ShowOnlyInnerProperties))
	TObjectPtr<UObject> Settings;

	/**
	 * A step of this registered type, outered to Outer, with fresh settings.
	 *
	 * Null with a sentence when the type is not registered. The panel, the toolset and anything else
	 * that adds a step goes through here, so none of them can make one with settings of the wrong class.
	 */
	static UMeshRegisteredPostStep* Create(UObject* Outer, FName TypeId, FString& OutError);

	/** The registration behind this step, or null when its plugin is absent. */
	const FMeshForgePostStepType* FindType() const;

	virtual FName GetProviderId() const override { return StepType; }
	virtual FString DescribeCost() const override { return FString(); }
	virtual FString Validate() const override;
	virtual FString Signature() const override;
	virtual void Run(const FMeshPostJob& Job, FMeshPostResult& OutResult) const override;

	virtual bool IsNativeStep() const override { return true; }
	virtual bool AcceptsNativeInput(const UClass* InputClass) const override;
	virtual UObject* CreateNativeOutput(UObject* Input, FMeshPostNativeContext& Context, FString& Error) const override;
	virtual FString GetNativeOutputPrefix() const override { return FString(); }
	virtual FText GetStepDisplayName() const override;
	virtual bool IsOfferedInPicker() const override { return false; }

	virtual FMeshPipelineIO GetIO() const override
	{
		// Asset work: it needs something made before it, and hands on an asset rather than geometry.
		FMeshPipelineIO IO;
		IO.bNeedsMesh = true;
		return IO;
	}
};

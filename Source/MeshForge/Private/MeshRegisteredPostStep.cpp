#include "MeshRegisteredPostStep.h"

#include "MeshForge.h"
#include "MeshForgeExtensions.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"

UMeshRegisteredPostStep* UMeshRegisteredPostStep::Create(UObject* Outer, FName TypeId, FString& OutError)
{
	const FMeshForgeModule* Module = FMeshForgeModule::GetPtr();
	const FMeshForgePostStepType* Type = Module ? Module->FindPostStepType(TypeId) : nullptr;

	if (Type == nullptr)
	{
		OutError = FString::Printf(TEXT("No post step called '%s' is registered. Its plugin may not be enabled."),
			*TypeId.ToString());
		return nullptr;
	}

	UMeshRegisteredPostStep* Step = NewObject<UMeshRegisteredPostStep>(Outer, NAME_None, RF_Transactional);
	Step->StepType     = Type->Id;
	Step->OwningPlugin = Type->OwningPlugin;

	// Outered to the step so it is saved inside the definition with it, and duplicated with it when a
	// run takes its copy.
	Step->Settings = NewObject<UObject>(Step, Type->SettingsClass, NAME_None, RF_Transactional);
	return Step;
}

const FMeshForgePostStepType* UMeshRegisteredPostStep::FindType() const
{
	const FMeshForgeModule* Module = FMeshForgeModule::GetPtrIfLoaded();
	return Module ? Module->FindPostStepType(StepType) : nullptr;
}

FText UMeshRegisteredPostStep::GetStepDisplayName() const
{
	if (const FMeshForgePostStepType* Type = FindType())
	{
		return Type->DisplayName;
	}

	// Named after what is missing rather than after this class, which would read as a step in itself.
	return FText::FromString(FString::Printf(TEXT("%s (needs %s)"),
		*StepType.ToString(), OwningPlugin.IsEmpty() ? TEXT("its plugin") : *OwningPlugin));
}

FString UMeshRegisteredPostStep::Validate() const
{
	const FMeshForgePostStepType* Type = FindType();

	if (Type == nullptr)
	{
		return FString::Printf(TEXT("'%s' comes from %s, which is not enabled in this project."),
			*StepType.ToString(), OwningPlugin.IsEmpty() ? TEXT("a plugin") : *OwningPlugin);
	}

	if (Settings == nullptr || !Settings->IsA(Type->SettingsClass))
	{
		return FString::Printf(TEXT("'%s' has lost its settings. Remove the step and add it again."),
			*Type->DisplayName.ToString());
	}

	return Type->Validate ? Type->Validate(Settings) : FString();
}

FString UMeshRegisteredPostStep::Signature() const
{
	// The base walks this class's properties, which would hash the settings as an object path and miss
	// every edit made inside it. The settings' own properties are what decide the output.
	FString Result = FString::Printf(TEXT("%s[%s]:"), *GetClass()->GetName(), *StepType.ToString());

	if (Settings != nullptr)
	{
		const UObject* Defaults = Settings->GetClass()->GetDefaultObject();

		for (TFieldIterator<FProperty> It(Settings->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}

			// The same rule as a pipeline's own settings: a setting another plugin adds later is left out while
			// it holds its default, so shipping it does not mark every saved step stale.
			if (Property->HasMetaData(TEXT("SignatureOmitsDefault")) && Property->Identical_InContainer(Settings, Defaults))
			{
				continue;
			}

			FString Value;
			Property->ExportTextItem_Direct(Value, Property->ContainerPtrToValuePtr<void>(Settings), nullptr, nullptr, PPF_None);
			Result += FString::Printf(TEXT("%s=%s;"), *Property->GetName(), *Value);
		}
	}

	return Result;
}

void UMeshRegisteredPostStep::Run(const FMeshPostJob& Job, FMeshPostResult& OutResult) const
{
	// Nothing on the worker. The work is asset work and happens in CreateNativeOutput, on the game
	// thread, once everything before it has imported.
	OutResult.MeshGlb         = Job.MeshGlb;
	OutResult.bKeepPlacement  = true;
	OutResult.ConsumedCredits = 0;
}

bool UMeshRegisteredPostStep::AcceptsNativeInput(const UClass* InputClass) const
{
	const FMeshForgePostStepType* Type = FindType();

	if (Type == nullptr || InputClass == nullptr)
	{
		return false;
	}

	if (Type->InputClasses.Num() == 0)
	{
		return true;
	}

	return Type->InputClasses.ContainsByPredicate([InputClass](const UClass* Accepted)
	{
		return Accepted != nullptr && InputClass->IsChildOf(Accepted);
	});
}

UObject* UMeshRegisteredPostStep::CreateNativeOutput(UObject* Input, FMeshPostNativeContext& Context, FString& Error) const
{
	const FString Refusal = Validate();
	if (!Refusal.IsEmpty())
	{
		Error = Refusal;
		return nullptr;
	}

	const FMeshForgePostStepType* Type = FindType();

	if (Input == nullptr || !AcceptsNativeInput(Input->GetClass()))
	{
		Error = FString::Printf(TEXT("'%s' cannot work on %s."),
			*Type->DisplayName.ToString(), Input ? *Input->GetClass()->GetName() : TEXT("a missing asset"));
		return nullptr;
	}

	FMeshForgePostStepContext Step;
	Step.Input              = Input;
	Step.InputMadeFrom      = Context.InputMadeFrom.IsNull() ? nullptr : Context.InputMadeFrom.TryLoad();
	Step.Settings           = Settings;
	Step.DefinitionName     = Context.DefinitionName;
	Step.DefinitionPath     = Context.DefinitionPath;
	Step.Prompt             = Context.Prompt;
	Step.OutputPackagePath  = FPackageName::GetLongPackagePath(Context.AssetPath);
	Step.SuggestedAssetName = FPackageName::GetShortName(Context.AssetPath);

	FMeshForgePostStepOutcome Outcome;
	Type->Run(Step, Outcome);

	if (!Outcome.Error.IsEmpty() || Outcome.Asset == nullptr)
	{
		Error = Outcome.Error.IsEmpty()
			? FString::Printf(TEXT("'%s' made nothing and gave no reason."), *Type->DisplayName.ToString())
			: Outcome.Error;
		return nullptr;
	}

	Context.Summary = Outcome.Summary;
	return Outcome.Asset;
}

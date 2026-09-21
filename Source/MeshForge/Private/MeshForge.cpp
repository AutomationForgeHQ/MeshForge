#include "MeshForge.h"

#if WITH_FORGE_KEYS
#include "ForgeKeyRegistry.h"
#endif
#include "MeshCredentialStore.h"
#include "MeshDef.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "MeshForgeSettings.h"
#include "MeshForgeSubsystem.h"
#include "IMeshProvider.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY(LogMeshForge);

#define LOCTEXT_NAMESPACE "FMeshForgeModule"

namespace MeshForgeConsole
{
	/**
	 * Acting on settings cannot happen on the settings page.
	 *
	 * UFUNCTION(CallInEditor) buttons do not render on a UDeveloperSettings page: the details
	 * customization drops archetype objects before deciding whether to draw them, and a settings
	 * panel edits the CDO, which is one. Console commands are the cheapest surface that actually
	 * works, and they suit CI and headless runs besides.
	 */

	static FName ResolveProvider(const TArray<FString>& Args)
	{
		if (Args.Num() > 0 && !Args[0].IsEmpty())
		{
			return FName(*Args[0]);
		}

		return GetDefault<UMeshForgeSettings>()->DefaultProviderId;
	}

	static void ListProviders(const TArray<FString>& Args)
	{
		FMeshForgeModule* Module = FMeshForgeModule::GetPtr();
		if (Module == nullptr)
		{
			UE_LOG(LogMeshForge, Error, TEXT("MeshForge module is not loaded."));
			return;
		}

		const TArray<FName> Ids = Module->GetProviderIds();

		if (Ids.Num() == 0)
		{
			// Not an error. MeshForge ships no provider of its own, so an install with no add-on
			// enabled lands here, and saying "no providers" as a failure would send somebody looking
			// for a broken registry instead of an unchecked box.
			UE_LOG(LogMeshForge, Display,
				TEXT("No mesh providers are registered. Enable a provider plugin - MeshForgeTrellis for a ")
				TEXT("local runner - and it will appear here."));
			return;
		}

		for (const FName Id : Ids)
		{
			TSharedPtr<IMeshProvider> Provider = Module->FindProvider(Id);
			if (!Provider.IsValid())
			{
				continue;
			}

			FString Reason;
			const bool bAvailable = Provider->IsAvailable(Reason);
			const FMeshProviderCaps Caps = Provider->GetCaps();

			UE_LOG(LogMeshForge, Display, TEXT("  %s (%s) - %s%s | %s, %s"),
				*Id.ToString(),
				*Provider->GetDisplayName(),
				bAvailable ? TEXT("ready") : TEXT("not ready: "),
				bAvailable ? TEXT("") : *Reason,
				Caps.bIsLocal ? TEXT("local") : TEXT("hosted"),
				Caps.bIsMetered ? TEXT("metered") : TEXT("free"));
		}
	}

	static void TestConnection(const TArray<FString>& Args)
	{
		FMeshForgeModule* Module = FMeshForgeModule::GetPtr();
		if (Module == nullptr)
		{
			return;
		}

		const FName Id = ResolveProvider(Args);
		TSharedPtr<IMeshProvider> Provider = Module->FindProvider(Id);

		if (!Provider.IsValid())
		{
			UE_LOG(LogMeshForge, Error, TEXT("No provider called '%s'. Run MeshForge.ListProviders."),
				*Id.ToString());
			return;
		}

		UE_LOG(LogMeshForge, Display, TEXT("Testing '%s'..."), *Id.ToString());

		Provider->TestConnection([Id](bool bSuccess, const FString& Message)
		{
			UE_LOG(LogMeshForge, Display, TEXT("%s: %s - %s"),
				*Id.ToString(), bSuccess ? TEXT("OK") : TEXT("FAILED"), *Message);
		});
	}

	static void ImportFile(const TArray<FString>& Args)
	{
		if (Args.Num() == 0)
		{
			UE_LOG(LogMeshForge, Error,
				TEXT("Usage: MeshForge.ImportFile <absolute path to .glb> [asset name]"));
			return;
		}

		UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get();
		if (Subsystem == nullptr)
		{
			UE_LOG(LogMeshForge, Error, TEXT("MeshForge's subsystem is not available."));
			return;
		}

		const FString Name = Args.Num() > 1 ? Args[1] : FString();

		// The project's defaults, because somebody typing a console command has not been offered a
		// place to choose anything else.
		const FMeshImportOutcome Outcome =
			Subsystem->ImportMeshFile(Args[0], Name, GetDefault<UMeshForgeSettings>()->DefaultFinish);

		if (!Outcome.bSuccess)
		{
			UE_LOG(LogMeshForge, Error, TEXT("%s"), *Outcome.Error);
			return;
		}

		UE_LOG(LogMeshForge, Display,
			TEXT("Imported %s: %d triangles, %d vertices, %d UV channels, %d collision primitives, Nanite %s, bounds %s"),
			*Outcome.Mesh.ToString(),
			Outcome.TriangleCount, Outcome.VertexCount, Outcome.SourceUVChannels,
			Outcome.CollisionPrimitives,
			Outcome.bNaniteEnabled ? TEXT("on") : TEXT("off"),
			*Outcome.BoundsSize.ToCompactString());

		UE_LOG(LogMeshForge, Display, TEXT("  %d materials, %d textures"),
			Outcome.Materials.Num(), Outcome.Textures.Num());
	}

	/**
	 * For every definition, compare each stage's stored input hash with the one it computes now.
	 *
	 * A stage saved as ready whose hash no longer matches would show as stale the next time its panel
	 * opens. Written to prove that changing how hashes are built left existing definitions alone, and
	 * kept because the same question comes up whenever that code is touched again.
	 */
	static void VerifyStageHashes(const TArray<FString>& Args)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UMeshDef::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(FName(Args.Num() > 0 ? *Args[0] : TEXT("/Game")));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Assets;
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Assets);

		int32 Compared = 0, Mismatched = 0;
		for (const FAssetData& Asset : Assets)
		{
			const UMeshDef* Def = Cast<UMeshDef>(Asset.GetAsset());
			if (Def == nullptr)
			{
				continue;
			}

			for (const TPair<EMeshStage, FMeshStageState>& Stage : Def->Stages)
			{
				if (Stage.Value.InputsHash.IsEmpty() || Stage.Value.Status != EMeshStageStatus::Ready)
				{
					continue;
				}

				++Compared;
				if (Stage.Value.InputsHash != Def->ComputeStageHash(Stage.Key))
				{
					++Mismatched;
					UE_LOG(LogMeshForge, Display, TEXT("  %s: %s saved ready, inputs now hash differently."),
						*Def->GetName(), *StaticEnum<EMeshStage>()->GetNameStringByValue(static_cast<int64>(Stage.Key)));
				}
			}
		}

		UE_LOG(LogMeshForge, Display, TEXT("Stage hashes: %d definition(s), %d ready stage(s) compared, %d no longer match."),
			Assets.Num(), Compared, Mismatched);
	}

	static FAutoConsoleCommand VerifyStageHashesCmd(
		TEXT("MeshForge.VerifyStageHashes"),
		TEXT("Report definitions whose stages were saved ready but whose inputs now hash differently. Optional argument: a content folder."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&VerifyStageHashes));

	static void RebuildThumbnails(const TArray<FString>& Args)
	{
		if (UMeshForgeSubsystem* Subsystem = UMeshForgeSubsystem::Get())
		{
			Subsystem->RebuildDefinitionThumbnails(Args.Num() > 0 ? Args[0] : FString());
		}
	}

	static FAutoConsoleCommand RebuildThumbnailsCmd(
		TEXT("MeshForge.RebuildThumbnails"),
		TEXT("Regenerate and save the Content Browser thumbnail of every Mesh Definition. Optional argument: a content folder, default /Game."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RebuildThumbnails));

	static FAutoConsoleCommand ListCmd(
		TEXT("MeshForge.ListProviders"),
		TEXT("List registered mesh providers and whether each is ready to generate."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&ListProviders));

	static FAutoConsoleCommand ImportFileCmd(
		TEXT("MeshForge.ImportFile"),
		TEXT("Import a .glb from disk and finish it. Args: <absolute path> [asset name]."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&ImportFile));

	static FAutoConsoleCommand TestCmd(
		TEXT("MeshForge.TestConnection"),
		TEXT("Test a provider. Optional argument: provider id. Defaults to the project's."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TestConnection));
}

FMeshForgeModule* FMeshForgeModule::GetPtr()
{
	// LoadModulePtr rather than GetModulePtr, deliberately. An add-on in the same loading phase can
	// start before MeshForge's module has, and GetModulePtr would hand back null on exactly those
	// runs - producing a provider that never appears, intermittently, with no error.
	return FModuleManager::Get().LoadModulePtr<FMeshForgeModule>(TEXT("MeshForge"));
}

FMeshForgeModule* FMeshForgeModule::GetPtrIfLoaded()
{
	return FModuleManager::GetModulePtr<FMeshForgeModule>(TEXT("MeshForge"));
}

void FMeshForgeModule::RegisterProvider(TSharedRef<IMeshProvider> Provider)
{
	const FName Id = Provider->GetProviderId();

	// Replace rather than refuse. A hot reload re-runs StartupModule without ShutdownModule having
	// run first, and refusing here would leave the stale instance in place - which is the harder
	// failure to diagnose of the two.
	const bool bReplaced = Providers.Contains(Id);
	Providers.Add(Id, Provider);

	UE_LOG(LogMeshForge, Log, TEXT("Provider '%s' %s."),
		*Id.ToString(), bReplaced ? TEXT("re-registered") : TEXT("registered"));

	// Offer this provider's key to the shared Keys page - if ForgeKeys happens to be installed.
	// MeshForge does not link it and does not require it: without it this is a null check and the
	// key is still set on MeshForge's own settings page, which is the path that always works.
#if WITH_FORGE_KEYS
	if (IForgeKeysModule* Keys = IForgeKeysModule::GetOrLoad())
	{
		const FString Service = Provider->GetCredentialServiceName();
		const FString Display = Provider->GetDisplayName();

		FForgeKeyProvider Key;
		Key.Id = FName(*FString::Printf(TEXT("MeshForge.%s"), *Id.ToString()));
		Key.DisplayName = FText::FromString(Display);
		Key.Owner = Provider->GetOwningPluginName();
		Key.Purpose = Provider->GetCredentialPurpose().IsEmpty()
			? FText::Format(
				LOCTEXT("MeshKeyPurpose", "Mesh generation through {0}. Without it this provider cannot be used."),
				FText::FromString(Display))
			: Provider->GetCredentialPurpose();
		Key.bOptional = Provider->IsCredentialOptional();
		Key.HelpUrl = Provider->GetCredentialHelpUrl();
		Key.VaultEntryName = FString::Printf(TEXT("MeshForge/%s"), *Service);
		Key.EnvironmentVariableName = FMeshCredentialStore::GetEnvironmentVariableName(Service);

		// The operations stay here, on MeshForge's own store. ForgeKeys holds no vault code.
		Key.IsSet    = [Service]() { return FMeshCredentialStore::Has(Service); };
		Key.Describe = [Service]() { return FMeshCredentialStore::DescribeSource(Service); };
		Key.Store    = [Service](const FString& Secret) { return FMeshCredentialStore::Set(Service, Secret); };
		Key.Clear    = [Service]() { return FMeshCredentialStore::Remove(Service); };

		// Weak, so a provider whose plugin unloaded mid-test cannot be called through a dangling handle.
		TWeakPtr<IMeshProvider> WeakProvider = Provider.ToSharedPtr();
		Key.Test = [WeakProvider](FForgeKeyTestResult Done)
		{
			if (TSharedPtr<IMeshProvider> Pinned = WeakProvider.Pin())
			{
				Pinned->TestConnection([Done](bool bSuccess, const FString& Message)
				{
					Done(bSuccess, FText::FromString(Message));
				});
			}
			else
			{
				Done(false, LOCTEXT("ProviderGone", "That provider is no longer loaded."));
			}
		};
		Keys->Registry().Register(MoveTemp(Key));
	}
#endif

	OnProvidersChanged.Broadcast();
}

void FMeshForgeModule::UnregisterProvider(FName ProviderId)
{
	if (Providers.Remove(ProviderId) > 0)
	{
		UE_LOG(LogMeshForge, Log, TEXT("Provider '%s' unregistered."), *ProviderId.ToString());
#if WITH_FORGE_KEYS
		if (IForgeKeysModule* Keys = IForgeKeysModule::GetIfLoaded())
		{
			Keys->Registry().Unregister(FName(*FString::Printf(TEXT("MeshForge.%s"), *ProviderId.ToString())));
		}
#endif
		OnProvidersChanged.Broadcast();
	}
}

TSharedPtr<IMeshProvider> FMeshForgeModule::FindProvider(FName ProviderId) const
{
	const TSharedPtr<IMeshProvider>* Found = Providers.Find(ProviderId);
	return Found ? *Found : nullptr;
}

TArray<FName> FMeshForgeModule::GetProviderIds() const
{
	TArray<FName> Ids;
	Providers.GetKeys(Ids);
	Ids.Sort(FNameLexicalLess());
	return Ids;
}

void FMeshForgeModule::RegisterPostStepType(const FMeshForgePostStepType& Type)
{
	// Refused with a sentence rather than asserted: the caller is another plugin, and a check() would
	// take the editor down for somebody else's typo.
	if (Type.Id.IsNone() || Type.SettingsClass == nullptr || !Type.Run)
	{
		UE_LOG(LogMeshForge, Error,
			TEXT("A post step from '%s' was not registered: it needs an id, a settings class and a Run function."),
			*Type.OwningPlugin);
		return;
	}

	const bool bReplaced = PostStepTypes.Contains(Type.Id);
	PostStepTypes.Add(Type.Id, MakeShared<FMeshForgePostStepType>(Type));

	UE_LOG(LogMeshForge, Log, TEXT("Post step '%s' from %s %s."),
		*Type.Id.ToString(), *Type.OwningPlugin, bReplaced ? TEXT("re-registered") : TEXT("registered"));

	OnPostStepTypesChanged.Broadcast();
}

void FMeshForgeModule::UnregisterPostStepType(FName Id)
{
	if (PostStepTypes.Remove(Id) > 0)
	{
		UE_LOG(LogMeshForge, Log, TEXT("Post step '%s' unregistered."), *Id.ToString());
		OnPostStepTypesChanged.Broadcast();
	}
}

const FMeshForgePostStepType* FMeshForgeModule::FindPostStepType(FName Id) const
{
	const TSharedRef<FMeshForgePostStepType>* Found = PostStepTypes.Find(Id);
	return Found ? &Found->Get() : nullptr;
}

TArray<const FMeshForgePostStepType*> FMeshForgeModule::GetPostStepTypes() const
{
	TArray<const FMeshForgePostStepType*> Types;
	for (const TPair<FName, TSharedRef<FMeshForgePostStepType>>& Pair : PostStepTypes)
	{
		Types.Add(&Pair.Value.Get());
	}
	Types.Sort([](const FMeshForgePostStepType& A, const FMeshForgePostStepType& B)
	{
		return A.DisplayName.CompareTo(B.DisplayName) < 0;
	});
	return Types;
}

void FMeshForgeModule::StartupModule()
{
	// Nothing to register. Every mesh provider is an add-on; see the class comment.
}

void FMeshForgeModule::ShutdownModule()
{
	Providers.Empty();
	PostStepTypes.Empty();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMeshForgeModule, MeshForge)

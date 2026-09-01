#include "MeshCredentialStore.h"

#include "MeshForge.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincred.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace MeshForgeCredentials
{
	/**
	 * Services that are not this plugin's, they are the family's.
	 *
	 * A Runpod key and a Hugging Face token are accounts a *person* has, not things a plugin owns, so
	 * naming their vault entries after whichever plugin happened to ask first was wrong: install a
	 * second plugin that rents GPUs and you are asked for the same key again, and now there are two
	 * copies to keep in step. These live under one family-wide name that every plugin agrees on.
	 *
	 * Agreeing on a *name* is not a dependency. Each plugin still carries its own vault code and works
	 * alone; they simply write to the same drawer.
	 */
	static const TCHAR* SharedNamespace = TEXT("AutomationForge");

	static FString SharedServiceFor(const FString& Service)
	{
		// Matched by suffix rather than by exact name, so every plugin's own service - KimodoRunpod,
		// TrellisRunpod, whatever the next one calls it - lands in the same drawer without this list
		// having to learn each new name. The plugin that asks first fills it in; the rest find it.
		if (Service.EndsWith(TEXT("Runpod"), ESearchCase::IgnoreCase))      { return TEXT("Runpod"); }
		if (Service.EndsWith(TEXT("HuggingFace"), ESearchCase::IgnoreCase)) { return TEXT("HuggingFace"); }
		return FString();
	}

	/** Vault entries are namespaced so they are recognisable in the Windows credential list. */
	static FString MakeTargetName(const FString& Service)
	{
		const FString Shared = SharedServiceFor(Service);

		return Shared.IsEmpty()
			? FString::Printf(TEXT("MeshForge/%s"), *Service)
			: FString::Printf(TEXT("%s/%s"), SharedNamespace, *Shared);
	}

	/**
	 * Where a shared key used to live, or empty when it never moved.
	 *
	 * Read-only and deliberately so. A key already in the old entry keeps working, and gets written to
	 * the new one the next time somebody saves it - which is a migration nobody has to perform and
	 * nothing that copies a secret around behind their back.
	 */
	static FString LegacyTargetName(const FString& Service)
	{
		return SharedServiceFor(Service).IsEmpty()
			? FString()
			: FString::Printf(TEXT("MeshForge/%s"), *Service);
	}

#if PLATFORM_WINDOWS
	/**
	 * A sibling plugin's entry for the same shared service, found by looking rather than by a list.
	 *
	 * **The family-wide drawer only helps for keys written after it existed.** A Hugging Face token
	 * entered once for MotionForge lives at `MotionForge/KimodoHuggingFace`; every other plugin looked
	 * in the shared drawer, found nothing, and asked for the same token again - which is exactly the
	 * duplication the shared name was introduced to stop. So a miss falls back to whatever entry this
	 * user already has for the same service, whichever plugin wrote it.
	 *
	 * Read-only, and it stays that way. Writing back into somebody else's entry would be one plugin
	 * tidying another's drawer, which is not a decision this code gets to make; writes still go to
	 * the shared name.
	 *
	 * Only ever matches a *shared* service - a Runpod key or a Hugging Face token, which are accounts
	 * a person has rather than things a plugin owns. A vendor key belongs to whoever asked for it and
	 * is never searched for.
	 */
	static FString FindSiblingTarget(const FString& Service)
	{
		const FString Shared = SharedServiceFor(Service);

		if (Shared.IsEmpty())
		{
			return FString();
		}

		DWORD Count = 0;
		PCREDENTIALW* Found = nullptr;

		// A null filter enumerates this user's own credentials and needs no elevation. Windows has no
		// suffix filter, so the matching happens here.
		if (!::CredEnumerateW(nullptr, 0, &Count, &Found) || Found == nullptr)
		{
			return FString();
		}

		FString Match;

		for (DWORD Index = 0; Index < Count; ++Index)
		{
			const PCREDENTIALW Entry = Found[Index];

			if (Entry == nullptr || Entry->TargetName == nullptr
				|| Entry->Type != CRED_TYPE_GENERIC || Entry->CredentialBlobSize == 0)
			{
				continue;
			}

			const FString Target(Entry->TargetName);

			if (Target.EndsWith(Shared, ESearchCase::IgnoreCase))
			{
				Match = Target;
				break;
			}
		}

		::CredFree(Found);
		return Match;
	}
#endif

}

FString FMeshCredentialStore::GetEnvironmentVariableName(const FString& Service)
{
	return FString::Printf(TEXT("MESHFORGE_%s_KEY"), *Service.ToUpper());
}

bool FMeshCredentialStore::IsVaultAvailable()
{
#if PLATFORM_WINDOWS
	return true;
#else
	// macOS Keychain and Linux libsecret backends are not written yet - those platforms are
	// environment-variable only, which still keeps the key out of the project folder.
	return false;
#endif
}

bool FMeshCredentialStore::Get(const FString& Service, FString& OutSecret)
{
	// Environment wins, so CI and headless runs never depend on an interactive user's vault.
	const FString FromEnv = FPlatformMisc::GetEnvironmentVariable(*GetEnvironmentVariableName(Service));
	if (!FromEnv.IsEmpty())
	{
		OutSecret = FromEnv;
		return true;
	}

#if PLATFORM_WINDOWS
	FString Target = MeshForgeCredentials::MakeTargetName(Service);

	// A key stored before these two moved to the family-wide name is still a key the user gave us.
	const FString Legacy = MeshForgeCredentials::LegacyTargetName(Service);
	if (!Legacy.IsEmpty())
	{
		PCREDENTIALW Probe = nullptr;
		const bool bHasCurrent = ::CredReadW(*Target, CRED_TYPE_GENERIC, 0, &Probe) && Probe;
		if (Probe) { ::CredFree(Probe); }

		if (!bHasCurrent)
		{
			Target = Legacy;
		}
	}

	// Still nothing? Somebody else in the family may already hold this account's key.
	{
		PCREDENTIALW Probe = nullptr;
		const bool bHasTarget = ::CredReadW(*Target, CRED_TYPE_GENERIC, 0, &Probe) && Probe;
		if (Probe) { ::CredFree(Probe); }

		if (!bHasTarget)
		{
			const FString Sibling = MeshForgeCredentials::FindSiblingTarget(Service);

			if (!Sibling.IsEmpty())
			{
				// Said once per service, not on every read. This is asked whenever anything checks
				// readiness, which on a panel is every few seconds, and a true line repeated forty
				// times is how a log stops being read.
				static TSet<FString> Announced;

				if (!Announced.Contains(Service))
				{
					Announced.Add(Service);

					UE_LOG(LogMeshForge, Log,
						TEXT("Using the '%s' credential for '%s' - the same account, which this "
							 "family shares."),
						*Sibling, *Service);
				}

				Target = Sibling;
			}
		}
	}


	PCREDENTIALW Credential = nullptr;
	if (::CredReadW(*Target, CRED_TYPE_GENERIC, 0, &Credential) && Credential)
	{
		// CredentialBlob is raw bytes, stored here as UTF-8 and not null terminated.
		const int32 ByteCount = static_cast<int32>(Credential->CredentialBlobSize);
		if (ByteCount > 0 && Credential->CredentialBlob)
		{
			FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Credential->CredentialBlob), ByteCount);
			OutSecret = FString(Converter.Length(), Converter.Get());
		}
		else
		{
			OutSecret.Reset();
		}

		::CredFree(Credential);
		return !OutSecret.IsEmpty();
	}
#endif

	OutSecret.Reset();
	return false;
}

bool FMeshCredentialStore::Set(const FString& Service, const FString& Secret)
{
	if (Secret.IsEmpty())
	{
		return Remove(Service);
	}

#if PLATFORM_WINDOWS
	const FString Target = MeshForgeCredentials::MakeTargetName(Service);
	const FTCHARToUTF8 Converter(*Secret);

	CREDENTIALW Credential = {};
	Credential.Type = CRED_TYPE_GENERIC;
	Credential.TargetName = const_cast<LPWSTR>(*Target);
	Credential.CredentialBlobSize = static_cast<DWORD>(Converter.Length());
	Credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<ANSICHAR*>(Converter.Get()));

	// LOCAL_MACHINE keeps the entry to this user on this machine and survives logout. Deliberately
	// not CRED_PERSIST_ENTERPRISE, which roams the credential with the user profile - that would put
	// the key on a domain server or synced profile, which is exactly what we are avoiding.
	Credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

	if (::CredWriteW(&Credential, 0))
	{
		UE_LOG(LogMeshForge, Log, TEXT("Stored credential for '%s' in the Windows credential vault."), *Service);
		return true;
	}

	UE_LOG(LogMeshForge, Error, TEXT("CredWriteW failed for '%s' (error %u)."), *Service, ::GetLastError());
	return false;
#else
	UE_LOG(LogMeshForge, Error,
		TEXT("No credential vault backend on this platform. Set the %s environment variable instead."),
		*GetEnvironmentVariableName(Service));
	return false;
#endif
}

bool FMeshCredentialStore::Remove(const FString& Service)
{
#if PLATFORM_WINDOWS
	const FString Target = MeshForgeCredentials::MakeTargetName(Service);
	if (::CredDeleteW(*Target, CRED_TYPE_GENERIC, 0))
	{
		UE_LOG(LogMeshForge, Log, TEXT("Removed credential for '%s'."), *Service);
		return true;
	}
#endif
	return false;
}

bool FMeshCredentialStore::Has(const FString& Service)
{
	FString Unused;
	const bool bFound = Get(Service, Unused);

	// Do not leave the secret sitting in a stack string longer than needed.
	Unused.Empty();
	return bFound;
}

FString FMeshCredentialStore::DescribeSource(const FString& Service)
{
	const FString EnvName = GetEnvironmentVariableName(Service);
	if (!FPlatformMisc::GetEnvironmentVariable(*EnvName).IsEmpty())
	{
		return FString::Printf(TEXT("Configured (environment variable %s)"), *EnvName);
	}

	FString Unused;
	if (Get(Service, Unused))
	{
		Unused.Empty();
		return TEXT("Configured (OS credential vault)");
	}

	return IsVaultAvailable()
		? TEXT("Not configured")
		: FString::Printf(TEXT("Not configured - this platform has no vault backend, set %s"), *EnvName);
}

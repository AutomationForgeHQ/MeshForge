// API keys, kept out of the project folder entirely.

#pragma once

#include "CoreMinimal.h"

/**
 * Reads and writes provider secrets through the operating system's credential vault.
 *
 * The requirement is that a key must not survive a project copy, a commit, or a zip sent to someone
 * else. An ignored config file does not achieve that - it still exists inside the project directory,
 * so anyone who copies the folder wholesale copies the key with it, and one mistaken ignore rule
 * publishes it. Storing outside the project removes the possibility rather than mitigating it.
 *
 * Nothing here is a UPROPERTY. Reflected properties can be serialised into config, saved into an
 * asset, or dumped by a details panel, so the secret never becomes one - callers fetch it, use it,
 * and let it go.
 */
class MESHFORGE_API FMeshCredentialStore
{
public:

	/**
	 * Fetch a secret for a service, e.g. "Trellis".
	 *
	 * Checks the environment first so CI and headless runs can supply a key without touching the
	 * vault, then falls back to the OS store.
	 *
	 * @return false when neither source has one.
	 */
	static bool Get(const FString& Service, FString& OutSecret);

	/** Store or replace a secret. Returns false if the platform has no vault backend. */
	static bool Set(const FString& Service, const FString& Secret);

	/** Forget a secret. Returns true if one was removed. */
	static bool Remove(const FString& Service);

	/** True when Get would succeed - without returning the value. Safe to call from UI. */
	static bool Has(const FString& Service);

	/** Where a secret would be read from, for showing in settings without revealing it. */
	static FString DescribeSource(const FString& Service);

	/** The environment variable consulted for a service, e.g. MESHFORGE_UTHANA_KEY. */
	static FString GetEnvironmentVariableName(const FString& Service);

	/** True when this platform has a real vault rather than environment-only support. */
	static bool IsVaultAvailable();
};

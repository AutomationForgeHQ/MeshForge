// What any mesh generation service has to be able to do.

#pragma once

#include "CoreMinimal.h"
#include "MeshForgeTypes.h"

/** One generation request, fully resolved. Nothing here is a reference to be looked up later. */
struct FMeshSubmitRequest
{
	/** What to make. May be empty where an image carries the whole intent. */
	FString Prompt;

	/**
	 * The reference image, already decoded to PNG bytes.
	 *
	 * Bytes rather than a path or a UTexture2D, because the three providers this was designed
	 * against want it three different ways - a multipart upload, a base64 data URI, and a file on a
	 * shared volume - and every one of those is reachable from bytes. Handing over a path instead
	 * would mean each provider re-implements reading a texture out of the project.
	 *
	 * Empty where the request is text-only.
	 */
	TArray<uint8> ImagePng;

	/** Extra views of the same subject. Only read where bSupportsMultipleImages. */
	TArray<TArray<uint8>> AdditionalImagesPng;

	/**
	 * Pictures of the *surface* wanted, where the texture is overridden.
	 *
	 * Separate from the views above and not interchangeable with them: those are the object from
	 * other angles and describe its shape, these are what it should be made of. A provider that
	 * confused the two would texture a crate with a photograph of its own back.
	 */
	TArray<TArray<uint8>> TextureImagesPng;

	FString ModelId;

	/** Which of the requested takes this is. Carried through so results can be matched back. */
	int32 VariantIndex = 0;

	/** Seed, quality, budgets. Providers honour what their caps advertise and ignore the rest. */
	FMeshControl Control;
};

/** Outcome of submitting. */
struct FMeshSubmitResult
{
	bool bSuccess = false;
	FString JobId;
	FString Error;
	int32 VariantIndex = 0;

	/** The seed actually used, where the provider picked one. Recorded so a take stays reproducible. */
	int32 Seed = 0;
};

/** Outcome of polling a job. */
struct FMeshJobResult
{
	EMeshJobStatus Status = EMeshJobStatus::Pending;

	/** Set once Status is Succeeded. */
	FString MeshId;

	/** 0..1 where the provider reports it, for a progress bar. Negative means it does not. */
	float Progress = -1.f;

	/** Filled on success where the provider counts them. */
	int32 TriangleCount = 0;
	float GenerationSeconds = 0.f;

	/**
	 * What the provider says it actually charged. Negative where it does not say.
	 *
	 * **The only number here that is not a guess.** Everything else about price in this plugin is
	 * our own arithmetic over a published table, and a table can be wrong: Meshy's documentation
	 * says image-to-3d costs thirty credits plus ten for texturing, and a 4K textured task billed
	 * thirty. So where the vendor reports a figure it replaces ours, and the record says which it
	 * is holding.
	 */
	int32 ConsumedCredits = -1;

	FString Error;
};

using FOnMeshSubmitComplete   = TFunction<void(const FMeshSubmitResult&)>;
using FOnMeshJobPolled        = TFunction<void(const FMeshJobResult&)>;
using FOnMeshDownloadComplete = TFunction<void(bool /*bSuccess*/, const FString& /*Error*/)>;
using FOnMeshTestComplete     = TFunction<void(bool /*bSuccess*/, const FString& /*Message*/)>;

/**
 * A mesh generation service.
 *
 * Deliberately narrow: submit, poll, download, and enough metadata to drive a UI. Anything specific
 * to one vendor - REST versus a local container, how a job id is shaped, which model names exist,
 * whether the thing runs on this machine or a rented card - stays behind this line so the pipeline
 * above never learns about it.
 *
 * **The artifact is glTF, and that is the one thing this interface insists on.** Every provider in
 * this space emits GLB already, Unreal translates it natively through Interchange, and it carries
 * mesh, materials and textures in a single file - so a provider handing back anything else is
 * asking the pipeline to grow a second import path for no gain. A provider whose native format is
 * something else converts on its own side, where it knows what its own conventions mean.
 *
 * Every call is asynchronous and completes on the game thread.
 */
class MESHFORGE_API IMeshProvider
{
public:

	virtual ~IMeshProvider() = default;

	/** Stable identifier used in settings and mesh definitions, e.g. "Trellis". */
	virtual FName GetProviderId() const = 0;

	/** Human readable name for UI. */
	virtual FString GetDisplayName() const = 0;

	/**
	 * What this provider can do, so nothing above has to special-case it by name.
	 *
	 * Answered from a cache, because anything drawing a panel calls it every frame. That cache is
	 * why a runner somebody started a minute ago can still be reported as down: nothing had asked it
	 * since. Anything showing readiness should call RefreshState on a slow timer rather than
	 * trusting what it was told when it opened.
	 */
	virtual FMeshProviderCaps GetCaps() const = 0;

	/**
	 * True when the provider is ready to generate right now, with a sentence saying why not.
	 *
	 * Separate from being registered. A local runner needs its container up; a hosted one needs a
	 * credential. A provider that is registered but not ready is the normal state on a fresh
	 * checkout, and it deserves an explanation rather than a failure.
	 */
	virtual bool IsAvailable(FString& OutReason) const = 0;

	/**
	 * Re-read whatever this provider caches about its own readiness, then call back.
	 *
	 * Default is a no-op that completes immediately, which is correct for a provider whose caps are
	 * constants. Keep the work small - a single health call, not a shell out.
	 */
	virtual void RefreshState(TFunction<void()> OnDone) { OnDone(); }

	/**
	 * Do whatever it takes to become ready, then call back. May take minutes.
	 *
	 * **Different from IsAvailable, which only reports.** A hosted provider is either ready or it is
	 * missing a key, and there is nothing to do about the second from here - so the default answers
	 * from IsAvailable and completes at once. A provider that runs on hardware somebody owns is the
	 * interesting case: its container can be *started*, and refusing to generate because it happens
	 * to be stopped makes a person do by hand something the plugin knows how to do.
	 *
	 * Whatever this does must not block the game thread. Completes on the game thread.
	 */
	virtual void PrepareForWork(TFunction<void(bool /*bReady*/, const FString& /*Reason*/)> OnReady)
	{
		FString Reason;
		const bool bReady = IsAvailable(Reason);
		OnReady(bReady, Reason);
	}

	/** Model used when a definition names none. */
	virtual FString GetDefaultModelId() const { return FString(); }

	/** Extension the artifacts this provider downloads arrive with, without the dot. */
	virtual FString GetArtifactExtension() const { return TEXT("glb"); }

	/**
	 * The extension *this particular job* will come back as, where a provider varies it.
	 *
	 * **glTF is still the rule and this is the documented exception.** Quad topology is the case that
	 * forced it: at least one vendor emits quads only as FBX, and refusing that meant refusing quad
	 * meshes altogether - which is the one output a game artist most wants. The objection to FBX was
	 * always about an FBX *from a DCC package*, which arrives at an arbitrary scale with arbitrary
	 * axes; one from a generator is the same normalised mesh a glTF would have been.
	 *
	 * Default answers the provider-wide extension, which is right for every provider that has one.
	 */
	virtual FString GetArtifactExtensionForJob(const FString& JobId) const
	{
		return GetArtifactExtension();
	}

	// ---------------------------------------------------------------------------------------------
	// Credentials
	// ---------------------------------------------------------------------------------------------

	/** Service name this provider's secret is stored under in the credential vault. */
	virtual FString GetCredentialServiceName() const = 0;

	/** Where a person signs up for a key, shown beside the field that asks for one. */
	virtual FString GetCredentialHelpUrl() const { return FString(); }

	/** What this provider's key is for, where the generic sentence would mislead. */
	virtual FText GetCredentialPurpose() const { return FText(); }

	/**
	 * True when the provider works without a key - a runner on loopback, for instance, where
	 * demanding a token would be ceremony. Says so on the Keys page instead of implying a fault.
	 */
	virtual bool IsCredentialOptional() const { return false; }

	/**
	 * Which plugin this provider ships in, for the shared Keys page.
	 *
	 * **Not MeshForge, for anything that actually has a key.** The registration lives in the core,
	 * because that is where providers announce themselves - so every key was attributed to
	 * MeshForge, which is a core and spends nothing. Somebody uninstalling the plugin that owns a
	 * key would go looking for the wrong one.
	 *
	 * Defaults to MeshForge so a provider that forgets is merely unhelpful rather than wrong: a
	 * provider compiled into the core genuinely would belong to it.
	 */
	virtual FText GetOwningPluginName() const { return NSLOCTEXT("MeshForge", "OwnerCore", "MeshForge"); }

	/** True when a usable credential is available. */
	virtual bool HasCredential() const = 0;

	// ---------------------------------------------------------------------------------------------
	// Setup surface
	// ---------------------------------------------------------------------------------------------

	/**
	 * The name of the place a person sets this provider up, when it has one beyond a key.
	 *
	 * Empty for a hosted service, where an API key is the whole of it. A provider that runs on
	 * hardware somebody has to manage - a container to start, a GPU to rent - has more to say, and
	 * this is how it offers it without MeshForge knowing what a container is.
	 *
	 * MeshForge asks the provider rather than looking for a plugin by name, so a provider added
	 * later gets the same button with no change here. The one rule is the family's: an add-on may
	 * be absent, so an empty answer must remain perfectly ordinary.
	 */
	virtual FText GetSetupSurfaceLabel() const { return FText(); }

	/** Open that place. Called only when the label above is non-empty. */
	virtual void OpenSetupSurface() const {}

	// ---------------------------------------------------------------------------------------------
	// Generation
	// ---------------------------------------------------------------------------------------------

	/** Start one generation. */
	virtual void SubmitJob(const FMeshSubmitRequest& Request, FOnMeshSubmitComplete OnComplete) = 0;

	/** Ask whether a job has finished, and get its mesh id when it has. */
	virtual void PollJob(const FString& JobId, FOnMeshJobPolled OnComplete) = 0;

	/**
	 * Fetch a finished mesh to an absolute local path, as glTF.
	 *
	 * On a hosted provider this is the call that usually costs money.
	 */
	virtual void DownloadMesh(
		const FString& MeshId,
		const FString& AbsoluteLocalPath,
		FOnMeshDownloadComplete OnComplete) = 0;

	/**
	 * Fetch the preview video for a finished mesh. Only called where bProducesPreviewVideo.
	 *
	 * Worth having separately from the mesh: judging a take from a turntable is far cheaper than
	 * importing it, and on a metered provider the mesh download is the billed half.
	 */
	virtual void DownloadPreview(
		const FString& MeshId,
		const FString& AbsoluteLocalPath,
		FOnMeshDownloadComplete OnComplete)
	{
		OnComplete(false, TEXT("This provider does not produce previews."));
	}

	/** Ask the provider to forget a job and its files. Best effort; failure is not worth reporting. */
	virtual void ForgetJob(const FString& JobId) {}

	/** Cheapest possible authenticated call, for the settings Test Connection button. */
	virtual void TestConnection(FOnMeshTestComplete OnComplete) = 0;

	/**
	 * What one submission would cost on this provider, as a phrase for a person to read.
	 *
	 * Empty where the provider cannot say, which is the honest answer for most: a price is usually a
	 * property of somebody's billing plan rather than of the API. A vendor that publishes a table
	 * can answer and should, because the estimate is the last thing anyone reads before spending
	 * and "one billable generation" does not tell them whether that is a penny or a pound.
	 *
	 * Takes the pieces rather than a request, so an estimate can be quoted from a definition
	 * without building one - and without a caller wondering whether asking has already cost them
	 * something.
	 */
	virtual FString DescribeCost(const FMeshControl& Control, bool bHasImage, int32 AdditionalImages) const
	{
		return FString();
	}

	/**
	 * What one of this provider's credits costs in US dollars. Zero where it cannot be said.
	 *
	 * **Credits are not comparable between vendors and money is.** Thirty credits on one service and
	 * thirty on another are different quantities of money, so a ledger listing both in credits cannot
	 * be added up - which is the one thing anybody opens a ledger to do.
	 *
	 * Zero is the honest answer for a vendor that publishes no rate, and it must stay honest: a
	 * guessed rate is worse than a blank, because a blank prompts somebody to go and look while a
	 * wrong number does not. Providers whose plans bundle credits at varying rates should also
	 * answer zero rather than quote the headline price.
	 */
	virtual double GetUsdPerCredit() const { return 0.0; }

	/**
	 * Options that belong to this provider alone.
	 *
	 * `FMeshControl` can only carry what every provider has, which leaves out most of what makes
	 * one provider worth choosing over another. Declaring them here lets a details panel draw them
	 * when this provider is selected, and lets an agent discover them instead of guessing - values
	 * travel in `FMeshControl::Extra`, keyed by `FMeshProviderOption::Key`.
	 *
	 * **Read your own keys and ignore everything else.** A definition keeps its extras when its
	 * provider changes, so a provider will see keys belonging to another one, and refusing on that
	 * would break simply switching provider to compare.
	 */
	virtual TArray<FMeshProviderOption> GetOptions() const { return {}; }

	// ---------------------------------------------------------------------------------------------
	// Text to image
	//
	// Optional, and separate from generation on purpose.
	//
	// Providers split into two kinds here. Meshy and Tripo take a text prompt and make a mesh from
	// it directly; TRELLIS.2 cannot, because it is an image-to-3D model and nothing else. Rather
	// than let that difference reach the definition asset - where it would mean a prompt-only
	// definition works on one provider and is rejected by another - a provider that needs an image
	// may offer to make one.
	//
	// So the pipeline's rule is simple: if a request has text and no image, and the provider
	// requires an image, ask it to draw one first. Where it cannot, the request fails with a
	// sentence saying so rather than a schema error.
	// ---------------------------------------------------------------------------------------------

	/** True when GenerateConceptImage does anything. */
	virtual bool SupportsConceptImages() const { return false; }

	/**
	 * Draw a reference image for a prompt, so an image-only model can be driven from text.
	 *
	 * Writes PNG bytes on success. Runs where the provider runs, which for a local runner means no
	 * network call and no third-party account.
	 */
	virtual void GenerateConceptImage(
		const FString& Prompt,
		const FMeshControl& Control,
		TFunction<void(bool /*bSuccess*/, const TArray<uint8>& /*Png*/, const FString& /*Error*/)> OnComplete)
	{
		OnComplete(false, {}, TEXT("This provider cannot draw concept images."));
	}
};

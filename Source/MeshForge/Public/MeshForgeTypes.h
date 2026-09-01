// The vocabulary the whole pipeline speaks. Nothing here knows the name of a provider.

#pragma once

#include "CoreMinimal.h"
#include "MeshForgeTypes.generated.h"

class UMeshDef;

/** Where a definition is in its life. */
UENUM(BlueprintType)
enum class EMeshDefStatus : uint8
{
	/** Authored but never submitted. */
	Draft,

	/** Handed to a provider; no job id back yet. */
	Submitting,

	/** The provider is working. */
	Generating,

	/** At least one candidate finished and can be fetched. */
	Generated,

	/** An artifact is on disk. */
	Downloaded,

	/** A static mesh exists in the project. This is the end of the pipeline. */
	Imported,

	Failed,
};

/** Where one generation job is. */
UENUM(BlueprintType)
enum class EMeshJobStatus : uint8
{
	Pending,
	Running,
	Succeeded,
	Failed,
	Cancelled,
};

/**
 * How much to spend on one generation.
 *
 * Deliberately a coarse ladder rather than a resolution in voxels. Every provider has a quality
 * knob and none of them agree on its units - TRELLIS.2 counts voxel grid resolution, Meshy counts
 * credits and a boolean, Tripo counts a model name. A ladder is the only thing that survives being
 * translated into all three, and a definition authored against one provider should still mean
 * something when pointed at another.
 *
 * The seconds are indicative, measured on a rented datacentre card. A laptop is slower.
 */
UENUM(BlueprintType)
enum class EMeshQuality : uint8
{
	/** Fastest, roughest. For blocking out a scene. Seconds. */
	Draft,

	/** The sensible default: good enough to look at in context. */
	Standard,

	/** Slower, denser, better silhouettes and sharper features. */
	High,

	/** The provider's maximum. Minutes, and often not worth it for a prop. */
	Ultra,
};

/** What a mesh is going to be used for, which decides how it is finished on import. */
UENUM(BlueprintType)
enum class EMeshCollisionMode : uint8
{
	/** No collision primitives. Right for decoration nothing ever touches. */
	None,

	/** One box. Cheapest, and correct surprisingly often for crates, books and panels. */
	Box,

	/** One sphere. */
	Sphere,

	/** A single convex hull wrapped around the whole mesh. Good for rocks and blobs. */
	SingleConvex,

	/**
	 * Several convex hulls fitted to the shape.
	 *
	 * The only mode that gets a handle, a spout or a chair's legs right, and the only one that costs
	 * real seconds per mesh. Worth it for anything a character walks into or picks up.
	 */
	ConvexDecomposition,
};

/** Whether a generated mesh becomes a Nanite mesh. */
UENUM(BlueprintType)
enum class EMeshNaniteMode : uint8
{
	/** Never. Smallest, most predictable, works with every material. */
	Off,

	/** Always. */
	On,

	/**
	 * On when the mesh came back dense enough to be worth it.
	 *
	 * Generated meshes vary enormously - the same prompt at Draft and at Ultra differ by two orders
	 * of magnitude in triangles - so a fixed answer is wrong for one of them. The threshold lives in
	 * the project settings.
	 *
	 * Note this is not a free win: Nanite and a translucent or two-sided material do not mix, and a
	 * generated prop with a glass slot is exactly the case that breaks. See the family's note on
	 * Nanite trading one warning for another.
	 */
	Auto,
};

/**
 * What a provider can do, so nothing above has to special-case it by name.
 *
 * Deliberately not optional and deliberately wide. Every field here is a difference already known
 * to exist between TRELLIS.2, Meshy and Tripo - the three this interface was designed against - and
 * each one was, in some earlier draft, an assumption baked into the pipeline for whichever provider
 * happened to be written first.
 */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshProviderCaps
{
	GENERATED_BODY()

	// ---------------------------------------------------------------------------------------------
	// Inputs
	// ---------------------------------------------------------------------------------------------

	/** A text prompt alone can produce a mesh. False for TRELLIS.2, true for Meshy and Tripo. */
	UPROPERTY(BlueprintReadOnly, Category = "Inputs")
	bool bSupportsTextPrompt = false;

	/** An image alone can produce a mesh. */
	UPROPERTY(BlueprintReadOnly, Category = "Inputs")
	bool bSupportsImagePrompt = true;

	/**
	 * Text and image together, where the text refines what the image shows.
	 *
	 * Worth its own flag rather than inferring it from the two above: a provider can accept both
	 * fields and silently ignore one of them, which looks identical from here and is not the same
	 * thing at all.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Inputs")
	bool bSupportsTextWithImage = false;

	/** An image is required; text alone will be refused. */
	UPROPERTY(BlueprintReadOnly, Category = "Inputs")
	bool bRequiresImage = true;

	/** Several views of the same subject improve the result. */
	UPROPERTY(BlueprintReadOnly, Category = "Inputs")
	bool bSupportsMultipleImages = false;

	/** The provider cuts the subject out of its background itself. */
	UPROPERTY(BlueprintReadOnly, Category = "Inputs")
	bool bSupportsBackgroundRemoval = false;

	// ---------------------------------------------------------------------------------------------
	// Outputs
	// ---------------------------------------------------------------------------------------------

	/** Metallic and roughness come back, not just a colour map. */
	UPROPERTY(BlueprintReadOnly, Category = "Outputs")
	bool bSupportsPBR = true;

	/** Opacity is generated, so glass and foliage are possible. */
	UPROPERTY(BlueprintReadOnly, Category = "Outputs")
	bool bSupportsTransparency = false;

	/** A polygon budget is honoured rather than ignored. */
	UPROPERTY(BlueprintReadOnly, Category = "Outputs")
	bool bSupportsPolygonBudget = false;

	/** The texture resolution can be asked for. */
	UPROPERTY(BlueprintReadOnly, Category = "Outputs")
	bool bSupportsTextureSize = false;

	/** The provider can produce quad-dominant topology rather than raw triangles. */
	UPROPERTY(BlueprintReadOnly, Category = "Outputs")
	bool bSupportsQuadRemesh = false;

	/** A turntable or preview video comes back beside the mesh, for judging without importing. */
	UPROPERTY(BlueprintReadOnly, Category = "Outputs")
	bool bProducesPreviewVideo = false;

	/** Largest texture this provider will emit, in pixels. Zero when it does not say. */
	UPROPERTY(BlueprintReadOnly, Category = "Outputs")
	int32 MaxTextureSize = 0;

	// ---------------------------------------------------------------------------------------------
	// Behaviour
	// ---------------------------------------------------------------------------------------------

	/**
	 * A seed is honoured, so a definition is a recipe rather than a receipt.
	 *
	 * The difference matters more here than it looks: without it, a generated mesh file is precious
	 * and has to be kept forever, because the asset that describes it cannot recreate it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Behaviour")
	bool bSupportsSeed = false;

	/** More than one take can be asked for in a single submission. */
	UPROPERTY(BlueprintReadOnly, Category = "Behaviour")
	bool bSupportsVariants = false;

	/**
	 * Runs on hardware the user controls, so generation is unmetered.
	 *
	 * Drives defaults rather than logic: variants and retries are free here and billed elsewhere, so
	 * the UI can stop asking somebody to think about it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Behaviour")
	bool bIsLocal = false;

	/** Each generation costs money. The inverse of bIsLocal in practice, but stated rather than inferred. */
	UPROPERTY(BlueprintReadOnly, Category = "Behaviour")
	bool bIsMetered = true;

	/** Rough wall-clock for one Standard generation, in seconds. Zero when unknown. */
	UPROPERTY(BlueprintReadOnly, Category = "Behaviour")
	int32 TypicalSecondsPerMesh = 0;

	/** Quality steps this provider actually implements. Anything else is clamped to the nearest. */
	UPROPERTY(BlueprintReadOnly, Category = "Behaviour")
	TArray<EMeshQuality> SupportedQualities;

	/** Model identifiers a definition may name, for a picker. Empty means the provider has one model. */
	UPROPERTY(BlueprintReadOnly, Category = "Behaviour")
	TArray<FString> ModelIds;
};

/**
 * The knobs that make a generation reproducible, plus the ones that shape it.
 *
 * Providers honour whatever their capabilities advertise and ignore the rest. A provider that
 * cannot seed does not fail a request that asks for one - it logs and generates anyway, because
 * refusing would make one definition unusable across providers for no benefit.
 */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshControl
{
	GENERATED_BODY()

	/** Use Seed rather than letting the provider pick. Ignored where bSupportsSeed is false. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	bool bUseSeed = false;

	/** Variants walk this rather than repeating it, so one seed gives several reproducible takes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (EditCondition = "bUseSeed"))
	int32 Seed = 42;

	/** How much to spend. See EMeshQuality. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	EMeshQuality Quality = EMeshQuality::Standard;

	/**
	 * Triangle budget for the finished mesh. Zero leaves it to the provider.
	 *
	 * This is the single most useful knob in the struct and the one most often left alone. A raw
	 * generated mesh is a surface extracted from a voxel field: correct, dense, and shaped by the
	 * grid rather than by the object. Asking for a budget makes the provider decimate to it, which
	 * is what turns an accurate blob into something that belongs in a level.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = 0, UIMax = 500000))
	int32 TargetTriangles = 0;

	/** Texture resolution to ask for. Zero takes the provider's default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = 0, UIMax = 4096))
	int32 TextureSize = 0;

	/**
	 * Ask the provider to rebuild topology rather than emitting the extracted surface directly.
	 *
	 * Costs seconds and is almost always worth it: without it the triangulation follows the voxel
	 * grid, which shades badly and decimates worse.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	bool bRemesh = true;

	/** Cut the subject out of its background before generating. Ignored on an image that has alpha. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	bool bRemoveBackground = true;

	/** Provider-specific extras, for anything this struct has no field for. Rarely needed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control|Advanced")
	TMap<FString, FString> Extra;
};

/**
 * How an imported mesh is finished.
 *
 * Separate from FMeshControl on purpose: everything here happens *after* the provider is done, in
 * Unreal, on a file that already exists. It costs no money, it is reproducible without regenerating
 * anything, and it is the half of the pipeline a provider cannot do for you.
 */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshFinishSettings
{
	GENERATED_BODY()

	/** What collision to build. See EMeshCollisionMode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Finish")
	EMeshCollisionMode Collision = EMeshCollisionMode::ConvexDecomposition;

	/** Hulls to fit when decomposing. More is more accurate and more expensive at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Finish",
		meta = (ClampMin = 1, ClampMax = 32,
			EditCondition = "Collision == EMeshCollisionMode::ConvexDecomposition"))
	int32 ConvexHullCount = 8;

	/** Vertices per hull. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Finish",
		meta = (ClampMin = 6, ClampMax = 32,
			EditCondition = "Collision == EMeshCollisionMode::ConvexDecomposition"))
	int32 ConvexHullVertices = 16;

	/**
	 * Build a second UV set for baked lighting.
	 *
	 * On by default because a generated mesh arrives with exactly one UV set - the atlas the texture
	 * was baked into - and a static mesh with no lightmap UVs lights wrongly in any project not
	 * running Lumen everywhere. Costs build time, not runtime.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Finish")
	bool bGenerateLightmapUVs = true;

	/** Resolution of that lightmap UV set. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Finish",
		meta = (ClampMin = 16, ClampMax = 1024, EditCondition = "bGenerateLightmapUVs"))
	int32 LightmapResolution = 128;

	/** Whether the mesh becomes a Nanite mesh. See EMeshNaniteMode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Finish")
	EMeshNaniteMode Nanite = EMeshNaniteMode::Auto;

	/**
	 * How big the finished prop should be, in centimetres along its longest side. Zero leaves it
	 * at whatever size it imported as.
	 *
	 * **This is the only place real-world size can enter the pipeline, and it is why it is stated
	 * as a size rather than as a multiplier.** A model given one picture has no way to know whether
	 * it is looking at a mug or a water tower, so it normalises everything into a unit box - which
	 * glTF calls one metre and Unreal imports as 100cm. Every generated prop therefore arrives the
	 * same size regardless of what it is, and a multiplier would mean working out that fact first.
	 *
	 * A size does not: 60 gives a 60cm crate whatever the generator did. Sensible values are the
	 * ones you would measure - 10 for a mug, 60 for a crate, 200 for a door.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Finish", meta = (ClampMin = 0, UIMax = 1000))
	float TargetSizeCm = 0.f;

	/** Put the origin on the base of the bounding box rather than its centre, so props sit on floors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Finish")
	bool bOriginAtBase = true;

	// ---------------------------------------------------------------------------------------------
	// LODs
	// ---------------------------------------------------------------------------------------------

	/**
	 * How many LODs to build below LOD 0. Zero builds none.
	 *
	 * **Nanite is not an answer to this**, which is why the two settings sit next to each other and
	 * neither replaces the other. Nanite does nothing for a translucent or masked material, nothing
	 * on a platform without it, and nothing where a mesh is rendered by something that wants real
	 * LODs. A prop with Nanite on and no LODs is finished for exactly one target.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LODs", meta = (ClampMin = 0, ClampMax = 8))
	int32 LodCount = 0;

	/**
	 * What fraction of the previous LOD's triangles each level keeps.
	 *
	 * 0.5 halves each time, which is the conventional ladder. Applied per level rather than against
	 * LOD 0, so three levels at 0.5 give 50%, 25% and 12.5%.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LODs",
		meta = (ClampMin = 0.05, ClampMax = 0.95, EditCondition = "LodCount > 0"))
	float LodReduction = 0.5f;

	// ---------------------------------------------------------------------------------------------
	// Textures
	// ---------------------------------------------------------------------------------------------

	/**
	 * Cap every imported texture at this many pixels on its longest side. Zero keeps what arrived.
	 *
	 * Separate from whatever the generator was asked for, and deliberately so: a 4K bake is worth
	 * paying for even when the prop ships at 1024, because the detail survives into the smaller map
	 * far better than generating small does. This is the last word regardless of how the texture
	 * was made.
	 *
	 * Named for the import rather than called `MaxTextureSize`, because `FMeshProviderCaps` already
	 * has one of those meaning the opposite thing - the largest a provider *can* emit. Two fields a
	 * few hundred lines apart, same name, one a ceiling and one a capability, is a bug waiting for
	 * whoever reads only one of them.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Textures",
		meta = (ClampMin = 0, ClampMax = 8192))
	int32 MaxImportedTextureSize = 0;

	/**
	 * Leave textures uncompressed.
	 *
	 * Costs memory and is occasionally right: a mesh whose texture is about to be sent back to a
	 * provider for retexturing should not make a round trip through block compression first. The
	 * source art survives compression - Unreal keeps it inside the asset - so this is about what
	 * renders, not about what can be recovered.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Textures")
	bool bUncompressedTextures = false;

	/**
	 * Mark the base colour map as sRGB and the rest as linear.
	 *
	 * On by default and almost never worth changing. A metallic or roughness map read as sRGB is
	 * the most common reason a generated prop looks subtly wrong in a way nobody can place.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Textures")
	bool bCorrectColourSpaces = true;

};

/** One generated take. */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshCandidate
{
	GENERATED_BODY()

	/** The provider's job handle, for polling. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString JobId;

	/**
	 * The provider's handle for the finished result, for fetching.
	 *
	 * Distinct from JobId because on hosted providers they genuinely are different things, and the
	 * one that survives after the job is forgotten is this one.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString MeshId;

	/** Which of the requested takes this was. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	int32 VariantIndex = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	EMeshJobStatus Status = EMeshJobStatus::Pending;

	/** Recorded whether or not one was asked for, so a good take can be reproduced after the fact. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	int32 Seed = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FName ProviderId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString ModelId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	EMeshQuality Quality = EMeshQuality::Standard;

	/** Absolute path to the downloaded artifact, once there is one. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString LocalArtifactPath;

	/** Absolute path to a preview video, where the provider made one. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString LocalPreviewPath;

	/** As reported by the provider, before import. Zero when it did not say. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	int32 TriangleCount = 0;

	/** Wall-clock the provider spent, in seconds. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	float GenerationSeconds = 0.f;

	/** What the provider says it charged. -1 where it does not say, which is most of them. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	int32 ConsumedCredits = -1;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FDateTime GeneratedAt = FDateTime(0);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString Error;

	bool IsUsable() const { return Status == EMeshJobStatus::Succeeded; }
};

/**
 * Everything an import produced, reported back rather than assumed.
 *
 * Worth returning in full because a glTF import is not one asset: it is a mesh, some materials and
 * some textures, and the count of each is the first thing that goes wrong quietly. A mesh that
 * imported with no textures looks like a successful import until somebody drags it into a level.
 */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshImportOutcome
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Import")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "Import")
	TSoftObjectPtr<UStaticMesh> Mesh;

	UPROPERTY(BlueprintReadOnly, Category = "Import")
	TArray<TSoftObjectPtr<UMaterialInterface>> Materials;

	UPROPERTY(BlueprintReadOnly, Category = "Import")
	TArray<TSoftObjectPtr<UTexture>> Textures;

	/**
	 * Triangles the mesh actually has, read from its mesh description.
	 *
	 * Not LOD 0's render data: that reports whatever build has finished at the moment it is
	 * asked, and under Nanite it reports the fallback mesh instead - which is smaller by an
	 * order of magnitude and is not what the asset contains.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Import")
	int32 TriangleCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Import")
	int32 VertexCount = 0;

	/** UV channels the mesh arrived with, before a lightmap set was added. */
	UPROPERTY(BlueprintReadOnly, Category = "Import")
	int32 SourceUVChannels = 0;

	/** Collision primitives built. Zero when the mode was None, or when the build failed. */
	UPROPERTY(BlueprintReadOnly, Category = "Import")
	int32 CollisionPrimitives = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Import")
	bool bNaniteEnabled = false;

	/** LODs the finished mesh has, including LOD 0. One where none were asked for or none built. */
	UPROPERTY(BlueprintReadOnly, Category = "Import")
	int32 LodCount = 1;

	/** Bounding box of the finished mesh in Unreal units, for sanity-checking scale. */
	UPROPERTY(BlueprintReadOnly, Category = "Import")
	FVector BoundsSize = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Import")
	FString Error;

	/**
	 * Things that succeeded but not well.
	 *
	 * Kept separate from Error because they must not fail the import and must not be silent either:
	 * a mesh with no textures, or with lightmap UV generation refused, is usable and is also not
	 * what was asked for.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Import")
	TArray<FString> Warnings;
};

/**
 * An authoring spec - what a person or an agent says they want, before any pipeline state exists.
 *
 * Separate from the asset so that creating a definition and configuring one are the same operation
 * from the caller's side, and so an agent has one struct to fill rather than a sequence of setters.
 */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshDefSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FString Prompt;

	/** Absolute path to an image on disk, or empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FString SourceImagePath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	int32 Variants = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FName ProviderId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FString ModelId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FMeshControl Control;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FMeshFinishSettings Finish;
};

/** What one submission started, so a caller can follow it without holding the definitions. */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshBatchSubmission
{
	GENERATED_BODY()

	/** Empty means nothing was submitted. Always check this before believing the counts. */
	UPROPERTY(BlueprintReadOnly, Category = "Batch")
	FString BatchId;

	UPROPERTY(BlueprintReadOnly, Category = "Batch")
	int32 Submitted = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Batch")
	int32 Failed = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Batch")
	FString LastError;
};

/**
 * What kind of value a provider-specific option takes.
 *
 * Deliberately small. This exists so a details panel can draw the right widget and an agent can
 * validate a value before spending on a refusal - not to describe an arbitrary schema.
 */
UENUM(BlueprintType)
enum class EMeshOptionType : uint8
{
	Bool     UMETA(DisplayName = "Boolean"),
	Int      UMETA(DisplayName = "Integer"),
	Enum     UMETA(DisplayName = "One of a set"),
	Text     UMETA(DisplayName = "Free text"),

	// Guidance scales and blend weights are the common case, and rounding one to an integer is not
	// a smaller version of it - 0.9 and 1 mean different things to a sampler.
	Float    UMETA(DisplayName = "Number"),
};

/**
 * One option that belongs to a single provider.
 *
 * The pipeline is provider-agnostic, which means `FMeshControl` can only carry what *every*
 * provider has. That leaves the things which make a provider worth choosing - a pose mode, a
 * symmetry hint, where the origin sits, whether the hard-surface path is used - with nowhere to
 * live but an untyped map nobody can discover.
 *
 * So a provider declares them. The editor draws whatever is declared when that provider is
 * selected, an agent can list them before it sets one, and the values travel in
 * `FMeshControl::Extra` as they already could - the declaration is what makes them findable.
 *
 * **A provider must ignore an option it did not declare**, because a definition keeps its extras
 * when the provider changes and the alternative is a refusal nobody asked for.
 */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshProviderOption
{
	GENERATED_BODY()

	/** The key this is stored under in FMeshControl::Extra, and the vendor's own field name. */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	FName Key;

	UPROPERTY(BlueprintReadOnly, Category = "Option")
	FText DisplayName;

	/** What it does, and where it is worth changing. Shown as a tooltip and read by an agent. */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	FText Tooltip;

	UPROPERTY(BlueprintReadOnly, Category = "Option")
	EMeshOptionType Type = EMeshOptionType::Bool;

	/** The permitted values, for Enum. Empty otherwise. */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	TArray<FString> AllowedValues;

	/** What the provider does when this is unset. Never blank for Bool or Enum. */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	FString DefaultValue;

	UPROPERTY(BlueprintReadOnly, Category = "Option")
	int32 MinValue = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Option")
	int32 MaxValue = 0;

	/**
	 * The range for a Float option. Ignored for every other type.
	 *
	 * A separate pair rather than reusing the integer one, so a UI reading `MinValue` on an `Int`
	 * option is never handed a truncated float - and so a Float option whose range is 0..1 does not
	 * report a range of 0..0.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	float MinFloat = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Option")
	float MaxFloat = 0.0f;

	/**
	 * Quality steps this option applies to. Empty means all of them.
	 *
	 * Not decoration: on at least one provider an option that does not belong to the current mode
	 * is not refused, it silently changes the mode and doubles the price. Saying where an option
	 * applies is how a UI avoids offering it somewhere it will do harm.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	TArray<EMeshQuality> AppliesTo;
};


// -------------------------------------------------------------------------------------------------
// Stages
// -------------------------------------------------------------------------------------------------
//
// A definition used to be one shot: a prompt and an image in, a mesh out. It is a pipeline now, and
// every stage of it is independently re-runnable - draw the concept again without touching the mesh,
// re-import with different collision without paying for generation twice.
//
// That is only honest if the asset knows which of its outputs still belong together, which is what
// the input hash below is for.

UENUM(BlueprintType)
enum class EMeshStage : uint8
{
	/** Prompt to reference image. Skipped when somebody supplies their own picture. */
	Concept     UMETA(DisplayName = "Concept image"),

	/** Reference image to reference images: background removal, and where a model can, other views. */
	References  UMETA(DisplayName = "References"),

	/** Reference images to a mesh. The expensive one. */
	Mesh        UMETA(DisplayName = "Mesh"),

	/** Remesh, decimate, unwrap, bake - as steps we own rather than steps buried in a provider. */
	Post        UMETA(DisplayName = "Post-processing"),

	/** Into Unreal: collision, lightmap UVs, Nanite, LODs, texture settings, scale and pivot. */
	Import      UMETA(DisplayName = "Import"),
};

UENUM(BlueprintType)
enum class EMeshStageStatus : uint8
{
	/** Never run, and has nothing to show. */
	Empty   UMETA(DisplayName = "Not run"),

	/** Run, succeeded, and its inputs have not changed since. */
	Ready   UMETA(DisplayName = "Ready"),

	Running UMETA(DisplayName = "Running"),

	/**
	 * Has an output, and that output was made from different inputs than the stage now holds.
	 *
	 * The output is deliberately kept. A mesh costs minutes and money, and discarding one because a
	 * prompt was edited would be the pipeline throwing away the expensive thing to protect a
	 * cheap invariant. What must not happen is showing it as though it still matched.
	 */
	Stale   UMETA(DisplayName = "Stale"),

	Failed  UMETA(DisplayName = "Failed"),
};

/**
 * What one stage has done, and whether it still counts.
 *
 * `InputsHash` is the whole mechanism. Each stage hashes the inputs it was actually run with; a
 * stage whose current inputs hash differently has drifted from its own output, and so has every
 * stage after it. Without this a definition can show a mesh made from a picture that has since been
 * regenerated, with nothing on screen to say so - which is the same class of failure as an asset
 * that plays the wrong voice line because two halves were edited independently.
 */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshStageState
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Stage")
	EMeshStageStatus Status = EMeshStageStatus::Empty;

	/** When this stage last produced its current output. Zero where it never has. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Stage")
	FDateTime LastRunUtc = FDateTime(0);

	/** How long it took, for a panel that should be honest about what a re-run will cost. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Stage")
	float LastRunSeconds = 0.0f;

	/** Empty unless Status is Failed. A sentence, and where possible one naming what to do next. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Stage")
	FString Error;

	/** A hash of the inputs this stage's current output was made from. See the note above. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Stage")
	FString InputsHash;

	bool HasOutput() const
	{
		return Status == EMeshStageStatus::Ready
			|| Status == EMeshStageStatus::Stale;
	}
};

/** Where a background job has got to. */
UENUM(BlueprintType)
enum class EMeshForgeJobState : uint8
{
	Running   UMETA(DisplayName = "Running"),
	Succeeded UMETA(DisplayName = "Succeeded"),
	Failed    UMETA(DisplayName = "Failed"),
};

/**
 * One piece of work MeshForge is doing in the background.
 *
 * **These exist because the editor must not freeze, and it used to.** Drawing a concept image takes
 * between fifteen seconds and two minutes; running it on the game thread and pumping HTTP inside
 * the wait meant Unreal stopped redrawing for the whole of it, which reads as a hang rather than as
 * progress. Every long operation is now started and left to run, and this is what somebody looks at
 * to see that it is still going.
 *
 * A finished job stays in the list for a short while rather than vanishing, because a job that
 * completes and disappears in the same frame is indistinguishable from one that never started.
 */
USTRUCT(BlueprintType)
struct MESHFORGE_API FMeshForgeJob
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Job")
	FGuid Id;

	/** The definition being worked on. Weak, because a job can outlive somebody deleting the asset. */
	UPROPERTY(BlueprintReadOnly, Category = "Job")
	TWeakObjectPtr<UMeshDef> Definition;

	/** Kept separately so a finished job can still say what it was for after the asset has gone. */
	UPROPERTY(BlueprintReadOnly, Category = "Job")
	FString DefinitionName;

	UPROPERTY(BlueprintReadOnly, Category = "Job")
	EMeshStage Stage = EMeshStage::Concept;

	/** One phrase for a person: "Drawing a concept image". */
	UPROPERTY(BlueprintReadOnly, Category = "Job")
	FString Label;

	UPROPERTY(BlueprintReadOnly, Category = "Job")
	EMeshForgeJobState State = EMeshForgeJobState::Running;

	/** Empty unless State is Failed. */
	UPROPERTY(BlueprintReadOnly, Category = "Job")
	FString Error;

	/** Wall clock, for display. */
	UPROPERTY(BlueprintReadOnly, Category = "Job")
	FDateTime StartedUtc;

	/**
	 * Platform seconds at the start, and at the end once it has finished.
	 *
	 * Separate from StartedUtc because the wall clock can move - a machine waking from sleep, or an
	 * NTP correction - and an elapsed time that jumps backwards looks like a bug in the pipeline.
	 */
	double StartedSeconds = 0.0;
	double FinishedSeconds = 0.0;

	/** How long it has been going, or how long it took. */
	float Elapsed() const
	{
		const double End = (State == EMeshForgeJobState::Running)
			? FPlatformTime::Seconds()
			: FinishedSeconds;

		return static_cast<float>(FMath::Max(0.0, End - StartedSeconds));
	}
};

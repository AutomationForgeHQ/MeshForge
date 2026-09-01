// An image pipeline: the one kind of pipeline that does its own work.

#pragma once

#include "CoreMinimal.h"
#include "MeshForgePipeline.h"
#include "MeshImagePipeline.generated.h"

/** What to draw. Deliberately not the definition - a pipeline never learns what a mesh is. */
struct MESHFORGE_API FMeshImageJob
{
	/** What the picture should show. */
	FString Prompt;

	/**
	 * Pictures to work from, for a pipeline that edits rather than invents.
	 *
	 * Empty for text-to-image. A background remover, a multi-view generator or a future art-bible
	 * adapter reads these; a pipeline that ignores them is not doing anything wrong.
	 */
	TArray<TArray<uint8>> InputImages;
};

/** One picture that came back. */
struct MESHFORGE_API FMeshDrawnImage
{
	/** PNG, JPEG or WebP, as the provider produced it. Not re-encoded on the way through. */
	TArray<uint8> Bytes;

	/**
	 * Added to the asset name where a pipeline returns several at once.
	 *
	 * A multi-view call comes back as four pictures of one subject, and `_Back` in the name is the
	 * difference between a gallery somebody can read and four tiles called Concept 3 to Concept 6.
	 */
	FString Suffix;
};

/** What a draw produced, or why it produced nothing. */
struct MESHFORGE_API FMeshImageDrawResult
{
	TArray<FMeshDrawnImage> Images;

	/**
	 * These pictures are one subject seen from several angles, in view order, front first.
	 *
	 * **Off by default, and the default is the careful one.** Several pictures from a drawing model
	 * are usually several *attempts*, and promoting them all to reconstruction views would hand a
	 * model contradictory evidence nobody asked it to fuse - which is exactly how multi-view made
	 * results worse in every test here.
	 *
	 * A pipeline whose whole purpose is a genuine orbit sets this, and only then does the stage
	 * claim the extra view slots. The distinction is the pipeline's to declare because it is the
	 * only thing that knows which it produced.
	 */
	bool bIsViewSet = false;

	/** A sentence for a person. Empty on success. */
	FString Error;

	bool IsOk() const { return Error.IsEmpty() && Images.Num() > 0; }
};

/**
 * A pipeline that produces pictures, and knows how to produce them itself.
 *
 * **This is the one place the "a pipeline only declares, a provider does" rule is deliberately
 * broken, and the reason is worth writing down.** A provider interface can only carry what every
 * provider has in common, and for image generation that is almost nothing: a local diffusion model
 * wants steps, a size and a seed; Meshy wants a model name, an aspect ratio and a pose; an art
 * bible adapter wants none of those and a data asset instead. Routing all of that through one
 * `GenerateConceptImage(Prompt, Control)` call meant the pipeline's typed settings were read by
 * the panel, saved on the asset, hashed into staleness - and then silently dropped on the floor at
 * the moment they were supposed to matter.
 *
 * So the settings and the call that uses them live in the same class. A subclass ships with its
 * provider's plugin, reads its own properties, and talks to whatever it talks to.
 *
 * **Draw blocks.** Every caller is a button somebody pressed, drawing takes fifteen seconds to two
 * minutes, and an async version would need a state machine in the definition, in the panel and in
 * the agent toolset to save an editor that is already going to sit still. Pump HTTP inside it if
 * you are waiting on a service - never the core ticker, which re-enters the subsystem's own poll.
 */
UCLASS(Abstract, BlueprintType, EditInlineNew, meta = (DisplayName = "Image Pipeline"))
class MESHFORGE_API UMeshImagePipeline : public UMeshForgePipeline
{
	GENERATED_BODY()

public:

	/** Always Image. A subclass that wanted to be something else would not be this class. */
	virtual EMeshPipelineKind GetKind() const override final { return EMeshPipelineKind::Image; }

	/**
	 * Draw. Blocks until there are pictures or a reason there are none.
	 *
	 * Report failure through `Out.Error` rather than by returning nothing - a caller that gets an
	 * empty list and an empty error has to invent a sentence for the user, and the invented one is
	 * always worse than the one you have.
	 */
	virtual void Draw(const FMeshImageJob& Job, FMeshImageDrawResult& Out) const
		PURE_VIRTUAL(UMeshImagePipeline::Draw,
			Out.Error = TEXT("This image pipeline has no implementation."););
};

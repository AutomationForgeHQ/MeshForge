// How a plugin puts its own control in the Mesh tab's bar, and its own meshes beside the subject.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class SWidget;
class USceneComponent;
class UMeshDef;

/**
 * The viewer, as an extension is allowed to touch it.
 *
 * **Deliberately small.** An extension may ask what is on screen and put meshes beside it; it may
 * not choose the take, move the camera about, or change how anything is drawn. Those belong to the
 * viewer, because they are the same question whichever add-ons happen to be installed - and a
 * second plugin quietly re-framing the camera is the failure mode this interface exists to make
 * impossible.
 *
 * **Hold it weakly.** The viewer *is* the host and the viewer owns the extensions, so an extension
 * that kept a `TSharedRef` to it would close the ring and neither would ever be destroyed - the
 * preview scene, its components and the meshes they hold would outlive the window they were opened
 * for. Keep a `TWeakPtr<IMeshDefPreviewHost>` and pin it per call; a pin that fails means the
 * window has gone, and there is nothing left to do.
 */
class MESHFORGEEDITOR_API IMeshDefPreviewHost
{
public:

	virtual ~IMeshDefPreviewHost() = default;

	/** The definition on screen. Null only while the window is closing. */
	virtual UMeshDef* GetDefinition() const = 0;

	/** Two while the A/B seam is up, one otherwise. */
	virtual int32 NumSides() const = 0;

	/** What that side is showing - a UStaticMesh or a USkeletalMesh - or null when it shows nothing. */
	virtual UObject* GetSubject(int32 Side) const = 0;

	/**
	 * Draw these beside that side's subject, replacing whatever this extension put there before.
	 * An empty array takes its meshes away again.
	 *
	 * **A companion puts the viewer into true space, and that is the point of it.** Left alone the
	 * viewer normalises: it centres each subject on the origin and, when comparing, turns and scales
	 * B until it matches A - which is right when the question is *which of these two takes is
	 * better*, and a lie the moment anything else is in the picture. A companion is a ruler. A ruler
	 * you resize measures nothing, and neither does one standing next to something that has been
	 * resized to suit it.
	 *
	 * So while any companion is present, nothing is centred, turned or scaled: every mesh is drawn at
	 * its own origin, at its own size, with its own pivot - what a level would show with all of them
	 * dropped at 0, 0, 0. A garment wrapped around a character lands on that character exactly, with
	 * no arithmetic anywhere, because the solver already wrote it in the character's space. A garment
	 * that was never wrapped hangs wrong, at the wrong size, in the wrong place. That is the answer,
	 * not a defect in the picture.
	 *
	 * Components therefore go in at identity and are never moved. The viewer keeps them alive and
	 * registered from this call until the next one. Make them with `NewObject<>()` and no outer; do
	 * not register them yourself.
	 */
	virtual void SetCompanions(FName ExtensionId, int32 Side, const TArray<USceneComponent*>& Components) = 0;

	/**
	 * Frame the subject and everything beside it together, once.
	 *
	 * Rarely needed: the viewer already re-frames by itself when companions come or go, because that
	 * is when the space changes. For anything else, think twice - the camera is the person's, and a
	 * viewer that snatched it back on every refresh would do so while somebody was leaning into a
	 * seam.
	 */
	virtual void Reframe() = 0;

	/** Redraw. The viewport draws on demand, so a change nobody asks for stays off screen. */
	virtual void Redraw() = 0;
};

/**
 * One plugin's contribution to the Mesh tab.
 *
 * The extension owns its control outright - the viewer only gives it a slot in the bar and never
 * reads what is in it. That is the point: MeshForge has no business knowing what a character is,
 * and MeshForgeGarment has no business asking MeshForge to grow the concept.
 */
class MESHFORGEEDITOR_API IMeshDefPreviewExtension : public TSharedFromThis<IMeshDefPreviewExtension>
{
public:

	virtual ~IMeshDefPreviewExtension() = default;

	/**
	 * The control for the bar. Null adds nothing at all - not an empty slot, not a disabled button.
	 *
	 * Called once, when the viewer is built.
	 */
	virtual TSharedPtr<SWidget> MakeControl() = 0;

	/**
	 * The subject, the number of sides, or the definition changed.
	 *
	 * Companions survive it - they are attached to a side rather than to a mesh - so an extension
	 * that shows the same thing whatever is on screen need do nothing here. One that reads the
	 * definition puts its companions back from this call.
	 */
	virtual void OnPreviewChanged() {}
};

/** One kind of extension a plugin offers. Registering an id again replaces the earlier one. */
struct FMeshDefPreviewExtensionType
{
	/** Stable, and prefixed with the owning plugin - `MeshForgeGarment.Character` - so two cannot collide. */
	FName Id;

	/** Where it sits in the bar, left to right among the extensions. Lower first. */
	int32 Order = 0;

	/** Make one for a viewer. Called once per Mesh tab. Keep the host weakly - see IMeshDefPreviewHost. */
	TFunction<TSharedRef<IMeshDefPreviewExtension>(const TSharedRef<IMeshDefPreviewHost>&)> Make;
};

/**
 * Who has offered one.
 *
 * A plain list rather than a module interface: MeshForgeEditor is an editor module and everything
 * that would extend a viewport is an editor module too, so the no-link dance
 * `IMeshForgeExtensionsModule` does for post steps buys nothing here. A plugin that wants a control
 * in this bar is already looking at MeshForge's viewport.
 */
class MESHFORGEEDITOR_API FMeshDefPreviewExtensions
{
public:

	static void Register(const FMeshDefPreviewExtensionType& Type);

	/** Withdraw it. Call from ShutdownModule: the closure points into the caller's module. */
	static void Unregister(FName Id);

	/** Everything registered, in bar order. */
	static const TArray<FMeshDefPreviewExtensionType>& Get();
};

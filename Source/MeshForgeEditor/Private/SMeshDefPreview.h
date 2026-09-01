// An orbiting look at any mesh this definition has made, one at a time or two against each other.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "EditorViewportClient.h"
#include "SEditorViewport.h"
#include "AdvancedPreviewScene.h"

class UMeshDef;
class UStaticMesh;
class UStaticMeshComponent;
class SMeshCompareWipe;

/**
 * Where the camera is about the subject.
 *
 * **A separate object so two viewports can share one.** Comparing two takes means two preview
 * scenes, and two cameras that merely start the same drift apart on the first drag - after which
 * the wipe is showing two different angles and every difference between them is the camera's. Held
 * by shared pointer, both clients read and write the same numbers and cannot disagree.
 */
struct FMeshOrbitState
{
	FVector Pivot = FVector::ZeroVector;
	float Radius = 200.0f;
	float Yaw = 45.0f;
	float Pitch = -20.0f;

	/** How close and how far the wheel may go, from the subject's own size. */
	float MinRadius = 10.0f;
	float MaxRadius = 10000.0f;

	/**
	 * Bumped on every change.
	 *
	 * A client that shares this has no other way to notice that the *other* one moved the camera:
	 * nothing calls it, its own input never ran, and its view would stay where it was until
	 * somebody dragged inside it.
	 */
	uint32 Serial = 0;
};

/**
 * The viewport client. Owns the camera and the show flags, and nothing else.
 *
 * Separate from the widget because `FEditorViewportClient` wants to outlive a Slate rebuild, and
 * because the two things it does - framing a mesh and choosing how it is drawn - are the two things
 * the panel above needs to ask for.
 */
class FMeshDefPreviewClient : public FEditorViewportClient
{
public:
	FMeshDefPreviewClient(FAdvancedPreviewScene& InScene, const TSharedRef<SEditorViewport>& InViewport);

	/** Put the camera where the whole mesh is visible, whatever size it is. */
	void FrameMesh(UStaticMeshComponent* Component);

	/**
	 * The same, from bounds given rather than read.
	 *
	 * What the compare mode needs: two meshes of different sizes have to be framed *together*, or
	 * one of them fills the frame and the other is a speck, and the wipe compares the framing
	 * instead of the meshes.
	 */
	void FrameBounds(const FBoxSphereBounds& Bounds);

	/** Follow the same camera as another viewport, by sharing its numbers rather than copying them. */
	void ShareOrbit(const TSharedRef<FMeshOrbitState>& InOrbit);

	TSharedRef<FMeshOrbitState> GetOrbit() const { return Orbit; }

	/**
	 * Lit, unlit, wireframe, or one of the G-buffer channels.
	 *
	 * `BufferMode` is only read for VMI_VisualizeBuffer, where it names the channel - `BaseColor`,
	 * `Metallic`, `Roughness` and the rest are materials the engine ships, so the list is the
	 * vendor's rather than ours here too.
	 */
	void ApplyViewMode(EViewModeIndex Mode, FName BufferMode);

	// FEditorViewportClient
	virtual void Tick(float DeltaSeconds) override;

	/**
	 * Orbit and zoom only. Everything that would move the camera off the mesh is swallowed.
	 *
	 * This is a *turntable*, not a level viewport. A generated prop is judged by turning it over
	 * and leaning in, the way every 3D generation tool presents one - and the moment a fly camera
	 * lets somebody drift, they are lost in an empty scene with no way back but a keyboard shortcut
	 * they do not know. So the pan and fly bindings are refused rather than left to be discovered.
	 */
	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;

	virtual bool InputAxis(const FInputKeyEventArgs& Args) override;

private:
	/** Put the camera where the spherical coordinates say, looking at the pivot. */
	void ApplyOrbit();

	/** Say that the numbers changed, so a viewport sharing them follows on its next tick. */
	void Touch() { ++Orbit->Serial; }

	/**
	 * The orbit, driven here rather than by FEditorViewportClient.
	 *
	 * **That is the whole point.** Its orbit mode is a level-viewport camera with orbiting bolted
	 * on: the right mouse button still dollies, dragging still creeps the pivot, and the arc is a
	 * level designer's arc rather than a turntable's. Holding the numbers directly makes the
	 * behaviour exactly what a 3D generation tool does - drag turns, wheel zooms, nothing else
	 * moves anything.
	 */
	TSharedRef<FMeshOrbitState> Orbit = MakeShared<FMeshOrbitState>();

	/** The last Serial this client drew. Behind means the other viewport moved the camera. */
	uint32 AppliedSerial = MAX_uint32;

	FAdvancedPreviewScene* Scene = nullptr;
};

/**
 * The viewport widget: a preview scene, one static mesh component, and an orbit camera.
 *
 * Built on `FAdvancedPreviewScene` rather than hand-rolled, so the lighting, the floor, the
 * environment and the profile dropdown are the ones the Static Mesh editor uses. A generated prop
 * judged under different lighting than the rest of the project is judged wrongly, and matching
 * Epic's preview is free where reinventing it is not.
 */
class SMeshDefPreviewViewport : public SEditorViewport, public FGCObject
{
public:
	SLATE_BEGIN_ARGS(SMeshDefPreviewViewport) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMeshDefPreviewViewport() override;

	/**
	 * Show this mesh, or nothing when it is null.
	 *
	 * `bFrame` off leaves the camera alone, which is what the compare mode wants: it frames both
	 * meshes together afterwards, and a re-frame per side would leave whichever was set last
	 * deciding where the shared camera sits.
	 */
	void SetMesh(UStaticMesh* Mesh, bool bFrame = true);

	UStaticMesh* GetMesh() const;

	/**
	 * Where the subject sits and how big it is drawn.
	 *
	 * **Because a shared camera is not the same thing as an aligned picture.** Two takes of one prop
	 * come back at whatever scale and pivot the generator felt like - 190 x 18 x 84 cm against
	 * 13 x 98 x 44 for the same pistol - so the same camera renders one large and one small, offset
	 * from each other, and the wipe compares the generators' arbitrary units instead of the meshes.
	 * Moving the subject rather than the camera is what makes the two halves overlay.
	 */
	void SetSubjectTransform(const FTransform& Transform);

	TSharedPtr<FMeshDefPreviewClient> GetClient() const { return Client; }

	/** Triangles, vertices, materials and bounds of what is on screen, for the strip beneath it. */
	FText DescribeMesh() const;

	// FGCObject - the preview component is not owned by anything the GC can otherwise see.
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("SMeshDefPreviewViewport"); }

protected:
	// SEditorViewport. No toolbar override: the controls that matter here - which mesh, and how it
	// is drawn - live on the panel above, beside the rest of the stage's controls rather than in a
	// second place somebody has to find. (`MakeViewportToolbar` is final in 5.8 in any case;
	// `BuildViewportToolbar` is the hook if one is ever wanted.)
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;

private:
	TSharedPtr<FAdvancedPreviewScene> PreviewScene;
	TSharedPtr<FMeshDefPreviewClient> Client;
	TObjectPtr<UStaticMeshComponent> Component;
};

/**
 * The Mesh tab: a bar of controls, the viewport or a pair of them, and a stats strip.
 *
 * **It shows any mesh this definition has made, not only the last one imported.** A definition
 * accumulates takes, each one imported into its own folder, and the question somebody actually has
 * once there are three of them is *which is better* - which cannot be answered by a viewer that
 * only ever shows the newest, or by opening two static mesh editors and looking from two different
 * angles.
 *
 * So there are two modes. One mesh, chosen from a list of what this definition has produced; or
 * two, drawn at the same camera with a seam that drags between them. The view mode - lit, unlit,
 * wireframe, base colour, and the rest - belongs to the viewer rather than to either side, because
 * comparing a lit mesh against a wireframe one answers nothing.
 *
 * The empty state matters more than it looks. A definition spends most of its life without an
 * imported mesh, and a blank viewport says nothing about *why* - so this says which stage has not
 * run yet.
 */
class SMeshDefPreview : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMeshDefPreview) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UMeshDef>, Definition)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Re-read the definition. Called when a stage finishes or the asset changes under us. */
	void Refresh();

private:

	/** One mesh this definition has produced and that is still in the project. */
	struct FMeshChoice
	{
		TSoftObjectPtr<UStaticMesh> Mesh;

		/** The take folder this came out of, or "supplied" for a mesh somebody set by hand. */
		FText Label;

		/** Where it lives, for the tooltip - two takes of one prop have the same asset name. */
		FText Detail;

		/**
		 * When the package was last written, which is the only honest way to order these.
		 *
		 * A take id is the provider's task id or a GUID - neither sorts by time, so a list ordered by
		 * name puts them in an order that looks meaningful and is not.
		 */
		FDateTime Modified = FDateTime::MinValue();
	};

	/**
	 * Everything under this definition's generated folder that is a static mesh, plus its supplied
	 * mesh where there is one.
	 *
	 * Read from the asset registry rather than from the definition, because the definition only
	 * remembers the *last* import - every take before it is still in the project, still named after
	 * the take that made it, and unreachable from here until now.
	 */
	void GatherChoices();

	TSharedRef<SWidget> BuildChoiceMenu(bool bSideB);
	TSharedRef<SWidget> BuildViewModeMenu();

	void Choose(bool bSideB, TSoftObjectPtr<UStaticMesh> Mesh);
	void SetCompare(bool bOn);
	void Swap();

	/** Push the chosen meshes into the viewports, and frame them - together, when comparing. */
	void ApplyMeshes();

	/**
	 * What was on screen the last time the camera was framed.
	 *
	 * The camera is re-framed only when the *choice* changes. Refresh runs whenever any job in the
	 * editor changes state, and a viewer that re-framed on each of those would snatch the camera
	 * back to three-quarters-from-above while somebody was leaning into a seam.
	 */
	TWeakObjectPtr<UStaticMesh> FramedA;
	TWeakObjectPtr<UStaticMesh> FramedB;
	bool bFramedCompare = false;
	bool bFramedAlign = true;

	/** Push the view mode into both viewports, because it belongs to the viewer and not to a side. */
	void ApplyViewMode();

	/** What is chosen for a side, falling back to the definition's own imported mesh for A. */
	TSoftObjectPtr<UStaticMesh> Chosen(bool bSideB) const;

	FText ChoiceLabel(bool bSideB) const;
	FText ViewModeLabel() const;

	EVisibility CompareOnlyVisibility() const;
	EVisibility EmptyVisibility() const;
	EVisibility ViewportVisibility() const;
	FText EmptyMessage() const;
	FText StatsText() const;

	TWeakObjectPtr<UMeshDef> Definition;

	TSharedPtr<SMeshDefPreviewViewport> Viewport;
	TSharedPtr<SMeshDefPreviewViewport> ViewportB;
	TSharedPtr<SMeshCompareWipe> Wipe;

	TArray<FMeshChoice> Choices;

	TSoftObjectPtr<UStaticMesh> ChosenA;
	TSoftObjectPtr<UStaticMesh> ChosenB;

	bool bCompare = false;

	/**
	 * Fit B onto A: same centre, same apparent size.
	 *
	 * A is left alone and B is moved to it, so the left half is always the take at its own scale and
	 * "align" means one thing rather than depending on which side you are looking at.
	 *
	 * On by default and switchable, because the scale difference is itself a fact about the take -
	 * the stats strip keeps reporting both meshes' true dimensions either way, and says what B was
	 * scaled by rather than quietly resizing it.
	 */
	bool bAlign = true;

	/** What B was scaled by to match A. One when nothing was done. */
	float AlignScale = 1.0f;

	/** How far B was turned to match A. Reported, so the fit is never a silent guess. */
	FRotator AlignRotation = FRotator::ZeroRotator;

	EViewModeIndex ViewMode = VMI_Lit;

	/** Which G-buffer channel, when ViewMode is VMI_VisualizeBuffer. */
	FName BufferMode;
};

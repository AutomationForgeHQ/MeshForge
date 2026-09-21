// An orbiting look at any mesh this definition has made, one at a time or two against each other.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "EditorViewportClient.h"
#include "SEditorViewport.h"
#include "AdvancedPreviewScene.h"
#include "MeshDefPreviewExtension.h"

class UMeshDef;
class UStaticMesh;
class UStaticMeshComponent;
class USkeletalMesh;
class USkeletalMeshComponent;
class UMeshComponent;
class USceneComponent;
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

	/** Put the camera where the whole of this is visible, whatever size it is. */
	void FrameMesh(UPrimitiveComponent* Component);

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
	 * A UStaticMesh or a USkeletalMesh; anything else shows nothing. **Both, because a garment
	 * stops being a static mesh halfway through its own chain.** The skinning step turns the
	 * wrapped shirt into a skeletal mesh, and a viewer that only knew about static meshes could
	 * show every take of that garment except the finished one.
	 *
	 * `bFrame` off leaves the camera alone, which is what the compare mode wants: it frames both
	 * meshes together afterwards, and a re-frame per side would leave whichever was set last
	 * deciding where the shared camera sits.
	 */
	void SetMesh(UObject* Mesh, bool bFrame = true);

	UObject* GetMesh() const;

	/** The component drawing it: static or skeletal, or null when nothing is shown. */
	UMeshComponent* GetMeshComponent() const;

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

	/** Show or hide the preview scene's floor, in this viewer only - never in the shared preview profile. */
	void SetFloorVisible(bool bVisible);

	/**
	 * Draw these beside the subject on behalf of one extension, replacing that extension's last set.
	 *
	 * Held here rather than by the extension because the preview scene, the registration and the GC
	 * root all belong to this viewport, and an extension that owned any of the three would have to
	 * be told when the viewport goes away. It is told nothing: the components simply stop existing
	 * with the viewport that drew them.
	 */
	void SetCompanions(FName ExtensionId, const TArray<USceneComponent*>& Components);

	/**
	 * Take every extension's companions out, whoever put them there.
	 *
	 * For the viewport that goes dark when the A/B seam is put away: it stops being a side, so
	 * nothing asks it for companions again, and without this it would keep drawing the character
	 * it had - invisibly, and back on screen the moment somebody compared two takes of something
	 * else entirely.
	 */
	void ClearCompanions();

	/**
	 * Everything on screen, in world space: the subject where its transform puts it, and every
	 * companion beside it.
	 *
	 * What framing has to use once a character is standing behind a shirt - framing the shirt alone
	 * would put the head and the knees off screen, which is the one thing the character was added
	 * to show.
	 */
	FBoxSphereBounds GetVisibleBounds() const;

	/**
	 * Keeps a hidden floor hidden.
	 *
	 * The preview scene re-applies the shared asset viewer profile - floor on - whenever any asset viewer
	 * setting is broadcast, which happens while the editor is still opening. Hiding the floor once at
	 * construction was therefore undone before the panel was first seen, and it only stayed hidden after
	 * the checkbox was toggled on and off again. Every change also asks the viewport to redraw, because it
	 * draws on demand and would otherwise keep showing the floor it last drew.
	 */
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	/** Anybody put anything beside the subject? The answer decides which space the viewer works in. */
	bool HasCompanions() const { return Companions.Num() > 0; }

	/**
	 * Triangles, vertices, materials and bounds of what is on screen, for the strip beneath it.
	 *
	 * `bWithPivot` also reports where the bounds centre sits, which is only worth the width in true
	 * space - there it is the number that answers *why is it floating over there*, and without it
	 * the strip says a garment is the right size while it hangs a foot from the body.
	 */
	FText DescribeMesh(bool bWithPivot = false) const;

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
	/** Whichever of the two is drawing the subject now. Null when nothing is. */
	UMeshComponent* ActiveComponent() const;

	TSharedPtr<FAdvancedPreviewScene> PreviewScene;
	TSharedPtr<FMeshDefPreviewClient> Client;

	/**
	 * One of each, both registered from the start, and only one ever holding a mesh.
	 *
	 * Made once rather than swapped per subject: adding and removing components from a preview
	 * scene as somebody flips down a list of takes is work for no gain, and a component that has
	 * been unregistered and registered again loses the transform it was placed with.
	 */
	TObjectPtr<UStaticMeshComponent> Component;
	TObjectPtr<USkeletalMeshComponent> SkeletalComponent;

	/** What the subject was placed with, so a companion added later lands in the same space. */
	FTransform SubjectTransform = FTransform::Identity;

	/** By the extension that asked for them. */
	TMap<FName, TArray<TObjectPtr<USceneComponent>>> Companions;

	bool bFloorVisible = false;
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
class SMeshDefPreview : public SCompoundWidget, public IMeshDefPreviewHost
{
public:
	SLATE_BEGIN_ARGS(SMeshDefPreview) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UMeshDef>, Definition)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Re-read the definition. Called when a stage finishes or the asset changes under us. */
	void Refresh();

	// IMeshDefPreviewHost - what an add-on's control in this bar is allowed to ask for.
	virtual UMeshDef* GetDefinition() const override { return Definition.Get(); }
	virtual int32 NumSides() const override { return bCompare ? 2 : 1; }
	virtual UObject* GetSubject(int32 Side) const override;
	virtual void SetCompanions(FName ExtensionId, int32 Side, const TArray<USceneComponent*>& Components) override;
	virtual void Reframe() override;
	virtual void Redraw() override;

private:

	/** One mesh this definition has produced and that is still in the project. */
	struct FMeshChoice
	{
		/** A UStaticMesh or a USkeletalMesh: a garment is the first until it is skinned and the second after. */
		TSoftObjectPtr<UObject> Mesh;

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

	void Choose(bool bSideB, TSoftObjectPtr<UObject> Mesh);
	void SetCompare(bool bOn);
	void Swap();

	/** Push the chosen meshes into the viewports, and frame them - together, when comparing. */
	void ApplyMeshes();

	/**
	 * Decide which space the viewer is in, and place both subjects in it.
	 *
	 * **Two questions, two spaces, and what is on screen picks between them.** With nothing beside
	 * the subject the question is *which of these takes is better*, and the answer needs the takes
	 * normalised - centred, and fitted to each other on request - because two generators disagree
	 * about scale and pivot and that disagreement is not the thing being judged. Put a character in
	 * the picture and the question becomes *does this fit*, which is entirely about scale and pivot,
	 * and every one of those helpful adjustments becomes a lie. So then nothing is adjusted at all.
	 *
	 * Called whenever either could have changed: a new subject, or companions coming or going.
	 */
	void ApplySpace();

	/** True space: something is standing beside the subject, so nothing is centred, turned or scaled. */
	bool IsTrueSpace() const;

	/**
	 * What was on screen the last time the camera was framed.
	 *
	 * The camera is re-framed only when the *choice* changes. Refresh runs whenever any job in the
	 * editor changes state, and a viewer that re-framed on each of those would snatch the camera
	 * back to three-quarters-from-above while somebody was leaning into a seam.
	 */
	TWeakObjectPtr<UObject> FramedA;
	TWeakObjectPtr<UObject> FramedB;
	bool bFramedCompare = false;
	bool bFramedAlign = true;

	/** Which space the last placement used, so the camera is re-framed when it changes and not otherwise. */
	bool bFramedTrueSpace = false;

	/** Push the view mode into both viewports, because it belongs to the viewer and not to a side. */
	void ApplyViewMode();

	/** Push the floor setting into both viewports, for the same reason. */
	void ApplyFloor();

	/** What is chosen for a side, falling back to the definition's own imported mesh for A. */
	TSoftObjectPtr<UObject> Chosen(bool bSideB) const;

	FText ChoiceLabel(bool bSideB) const;
	FText ViewModeLabel() const;

	/** Why Align is greyed, when it is. */
	FText AlignTooltip() const;

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

	TSoftObjectPtr<UObject> ChosenA;
	TSoftObjectPtr<UObject> ChosenB;

	bool bCompare = false;

	/**
	 * The add-ons' controls, made once when the bar is built and kept for as long as it is.
	 *
	 * Kept rather than made per draw because an extension holds state a person set - which
	 * character is standing behind the garment - and a control rebuilt on every refresh would
	 * forget it.
	 */
	TArray<TSharedRef<IMeshDefPreviewExtension>> Extensions;

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

	/**
	 * How much bigger B's bounds are than A's, in true space, where nothing was scaled.
	 *
	 * Worked out when the subjects are placed and kept, not asked for by the stats strip: that text
	 * is a delegate read every frame, and measuring a take means sampling twenty thousand vertices.
	 */
	float TrueSpaceRatio = 1.0f;

	/** How far B was turned to match A. Reported, so the fit is never a silent guess. */
	FRotator AlignRotation = FRotator::ZeroRotator;

	EViewModeIndex ViewMode = VMI_Lit;

	/** Which G-buffer channel, when ViewMode is VMI_VisualizeBuffer. */
	FName BufferMode;
};

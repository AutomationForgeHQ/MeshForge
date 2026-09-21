#include "SMeshDefPreview.h"

#include "MeshDef.h"
#include "ForgeLibrary.h"
#include "MeshForgeSettings.h"
#include "MeshForgeEditorSettings.h"
#include "SMeshCompareWipe.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "MeshDescription.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshResources.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MeshForgeEditor"

namespace MeshDefPreviewPrivate
{
	/** One entry in the view-mode menu. */
	struct FViewModeEntry
	{
		EViewModeIndex Mode;

		/** Only for VMI_VisualizeBuffer: which G-buffer channel. */
		const TCHAR* Buffer;

		const TCHAR* Label;
		const TCHAR* Tip;
	};

	/**
	 * The modes worth having on a generated prop, and no more.
	 *
	 * **Curated rather than reflected.** The engine's full list runs to shader complexity, light
	 * complexity, LOD colouration and a dozen G-buffer channels that mean nothing about a mesh
	 * somebody is deciding whether to keep. These are the ones that answer a question about it:
	 * does the silhouette read, is the texture doing the work the geometry should be doing, is the
	 * normal map inverted, is the whole thing metal because the generator guessed.
	 */
	static const FViewModeEntry ViewModes[] =
	{
		{ VMI_Lit,                nullptr,           TEXT("Lit"),
		  TEXT("The prop as it would appear in the project.") },

		{ VMI_Unlit,              nullptr,           TEXT("Unlit"),
		  TEXT("Albedo with no lighting. What the texture actually contains, with none of the "
		       "lighting's flattery - baked-in shadows show up here and nowhere else.") },

		{ VMI_BrushWireframe,     nullptr,           TEXT("Wireframe"),
		  TEXT("Where the polygons went. A generated mesh spends its budget unevenly, and this is "
		       "the only view that shows a face made of four triangles.") },

		{ VMI_Lit_DetailLighting, nullptr,           TEXT("Detail Lighting"),
		  TEXT("Lighting on a neutral surface. Separates the shape from the texture - if detail "
		       "disappears here it was painted on, not modelled.") },

		{ VMI_LightingOnly,       nullptr,           TEXT("Lighting Only"),
		  TEXT("The lighting with no albedo at all.") },

		{ VMI_ReflectionOverride, nullptr,           TEXT("Reflections"),
		  TEXT("A mirror finish over the whole mesh. Shows the surface normals the way nothing "
		       "else does - a dent that is only in the normal map reads here.") },

		{ VMI_VisualizeBuffer,    TEXT("BaseColor"), TEXT("Base Color"),
		  TEXT("The base colour channel on its own.") },

		{ VMI_VisualizeBuffer,    TEXT("Metallic"),  TEXT("Metallic"),
		  TEXT("White is metal, black is not. Generators are prone to calling a whole prop metal, "
		       "which is a one-glance check here and a mystery anywhere else.") },

		{ VMI_VisualizeBuffer,    TEXT("Roughness"), TEXT("Roughness"),
		  TEXT("White is rough, black is a mirror.") },

		{ VMI_VisualizeBuffer,    TEXT("Specular"),  TEXT("Specular"),
		  TEXT("The specular channel on its own.") },

		{ VMI_VisualizeBuffer,    TEXT("WorldNormal"), TEXT("World Normal"),
		  TEXT("Surface normals as colour. An inverted or missing normal map is obvious here.") },

		{ VMI_VisualizeBuffer,    TEXT("AmbientOcclusion"), TEXT("Ambient Occlusion"),
		  TEXT("The occlusion channel on its own.") },
	};
}

namespace MeshDefPreviewPrivate
{
	/** How many vertices the skew estimate looks at. Enough to be stable, cheap on a 2M-triangle take. */
	static constexpr int32 SkewSamples = 20000;

	/** What a mesh looks like, reduced to the few numbers two takes of one prop can be matched on. */
	struct FSubject
	{
		FVector Centre = FVector::ZeroVector;
		FVector Extent = FVector::OneVector;
		float Radius = 1.0f;

		/**
		 * Which way the mass leans, per axis, as a fraction of the extent.
		 *
		 * The box alone cannot tell a pistol from a pistol turned end for end - both give the same
		 * three numbers. Where the vertices actually sit inside that box can: a grip and a magazine
		 * hang off one end, and that is what separates the four remaining half-turns.
		 */
		FVector Skew = FVector::ZeroVector;
	};

	/** The subject's box, whichever kind of mesh it is. Zero extent when it is neither. */
	static bool BoundsOf(const UObject* Mesh, FBoxSphereBounds& OutBounds)
	{
		if (const UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			OutBounds = Static->GetBounds();
			return true;
		}

		if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			OutBounds = Skeletal->GetBounds();
			return true;
		}

		return false;
	}

	/**
	 * Where the vertices sit, strided, for the skew.
	 *
	 * A static mesh is read from its mesh description and a skeletal one from the model the editor
	 * keeps - the authored data in both cases rather than the render data, for the same reason the
	 * counts are: under Nanite the render data is a fallback mesh an order of magnitude smaller
	 * than the asset, and this plugin has already reported that number wrongly once.
	 */
	static void SampleVertices(const UObject* Mesh, TFunctionRef<void(const FVector&)> Visit)
	{
		if (const UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			const FMeshDescription* Description = Static->GetMeshDescription(0);

			if (Description == nullptr)
			{
				return;
			}

			const auto Positions = Description->GetVertexPositions();
			const int32 Count = Description->Vertices().Num();
			const int32 Stride = FMath::Max(1, Count / SkewSamples);

			for (int32 Index = 0; Index < Count; Index += Stride)
			{
				const FVertexID Vertex(Index);

				if (Description->IsVertexValid(Vertex))
				{
					Visit(FVector(Positions[Vertex]));
				}
			}

			return;
		}

		const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh);
		const FSkeletalMeshModel* Model = Skeletal ? Skeletal->GetImportedModel() : nullptr;

		if (Model == nullptr || Model->LODModels.Num() == 0)
		{
			return;
		}

		const FSkeletalMeshLODModel& Lod = Model->LODModels[0];
		const int32 Stride = FMath::Max(1, static_cast<int32>(Lod.NumVertices) / SkewSamples);

		for (const FSkelMeshSection& Section : Lod.Sections)
		{
			for (int32 Index = 0; Index < Section.SoftVertices.Num(); Index += Stride)
			{
				Visit(FVector(Section.SoftVertices[Index].Position));
			}
		}
	}

	static FSubject Measure(UObject* Mesh)
	{
		FSubject Subject;

		FBoxSphereBounds Bounds;

		if (Mesh == nullptr || !BoundsOf(Mesh, Bounds))
		{
			return Subject;
		}

		Subject.Centre = Bounds.Origin;
		Subject.Extent = Bounds.BoxExtent.ComponentMax(FVector(KINDA_SMALL_NUMBER));
		Subject.Radius = FMath::Max(static_cast<float>(Bounds.SphereRadius), KINDA_SMALL_NUMBER);

		// Strided rather than every vertex: the centroid of a fifth of a million points is the same
		// answer as the centroid of a million, and this runs whenever a take is chosen.
		FVector Sum = FVector::ZeroVector;
		int32 Taken = 0;

		SampleVertices(Mesh, [&Sum, &Taken](const FVector& Position)
		{
			Sum += Position;
			++Taken;
		});

		if (Taken > 0)
		{
			const FVector Centroid = Sum / static_cast<double>(Taken);
			Subject.Skew = (Centroid - Subject.Centre) / Subject.Extent;
		}

		return Subject;
	}

	/**
	 * The right-angle turn that best puts From onto To.
	 *
	 * **Right angles only, and deliberately.** Two generators disagreeing about orientation disagree
	 * about an axis convention - Y-up against Z-up, or which way is forward - not by nineteen
	 * degrees. Searching the quarter turns is exact where it applies and visibly does nothing where
	 * it does not, which is a better failure than a principal-axis fit that answers every time and
	 * is quietly wrong on anything close to symmetrical.
	 */
	static FRotator BestRotation(const FSubject& From, const FSubject& To)
	{
		// Normalised by the longest side, so a match is about proportion rather than size.
		const FVector TargetShape = To.Extent / To.Extent.GetMax();

		FRotator Best = FRotator::ZeroRotator;
		double BestScore = TNumericLimits<double>::Max();

		for (int32 Yaw = 0; Yaw < 4; ++Yaw)
		{
			for (int32 Pitch = 0; Pitch < 4; ++Pitch)
			{
				for (int32 Roll = 0; Roll < 4; ++Roll)
				{
					const FRotator Candidate(Pitch * 90.0, Yaw * 90.0, Roll * 90.0);
					const FQuat Turn = Candidate.Quaternion();

					const FVector Shape = Turn.RotateVector(From.Extent).GetAbs();
					const FVector Skewed = Turn.RotateVector(From.Skew);

					const double ShapeError =
						(Shape / Shape.GetMax() - TargetShape).SizeSquared();

					// Weighted to matter only once the box has been matched: proportion decides which
					// axis is which, and the lean decides which end is which.
					const double SkewError = (Skewed - To.Skew).SizeSquared();

					const double Score = ShapeError + SkewError * 0.35;

					if (Score < BestScore)
					{
						BestScore = Score;
						Best = Candidate;
					}
				}
			}
		}

		return Best;
	}
}

// -------------------------------------------------------------------------------------------------
// Viewport client
// -------------------------------------------------------------------------------------------------

FMeshDefPreviewClient::FMeshDefPreviewClient(
	FAdvancedPreviewScene& InScene, const TSharedRef<SEditorViewport>& InViewport)
	: FEditorViewportClient(nullptr, &InScene, StaticCastSharedRef<SEditorViewport>(InViewport))
	, Scene(&InScene)
{
	SetViewMode(VMI_Lit);

	// Orbit rather than fly. A generated prop is examined by turning it over, not by walking around
	// it, and a fly camera in a viewport this small mostly gets lost.
	//
	// The orbit is driven by hand in ApplyOrbit rather than by this flag. Turning the flag on as
	// well made the two fight: the base class kept its own pivot and its own idea of how far away
	// the camera should be, and moving the mouse dollied.
	bUsingOrbitCamera = false;
	SetViewportType(LVT_Perspective);

	EngineShowFlags.SetGrid(false);
	EngineShowFlags.SetScreenPercentage(true);

	// The real-time flag is what makes the orbit smooth; without it the viewport only redraws on
	// input and reads as broken.
	SetRealtime(true);
}

void FMeshDefPreviewClient::Tick(float DeltaSeconds)
{
	FEditorViewportClient::Tick(DeltaSeconds);

	// The camera may have been moved by the *other* viewport, which shares these numbers and has
	// no way to reach into this one. Cheap: an integer compare per frame.
	if (Orbit->Serial != AppliedSerial)
	{
		ApplyOrbit();
	}

	if (Scene != nullptr)
	{
		Scene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
	}
}

void FMeshDefPreviewClient::ShareOrbit(const TSharedRef<FMeshOrbitState>& InOrbit)
{
	Orbit = InOrbit;

	// Behind by construction, so the next tick applies whatever the shared state already says.
	AppliedSerial = MAX_uint32;
}

void FMeshDefPreviewClient::FrameMesh(UPrimitiveComponent* InComponent)
{
	if (InComponent == nullptr)
	{
		return;
	}

	FrameBounds(InComponent->Bounds);
}

void FMeshDefPreviewClient::FrameBounds(const FBoxSphereBounds& Bounds)
{
	// Framed from the bounding sphere rather than the box, so a long thin prop and a cube both fill
	// the viewport instead of one of them sitting in the distance.
	const float Distance = FMath::Max(static_cast<float>(Bounds.SphereRadius) * 2.4f, 50.0f);

	// Orbits about the point it is told to look at, so this both frames the mesh and sets what the
	// mouse turns around. Doing it by hand with SetViewLocation leaves the orbit pivot at the world
	// origin, which on a prop whose pivot is on its base means it swings rather than turns.
	Orbit->Pivot  = Bounds.Origin;
	Orbit->Radius = Distance;

	// A three-quarter view, slightly above. Straight on hides the top face, and the top of a prop is
	// where a generated one usually goes wrong.
	Orbit->Yaw   = 45.0f;
	Orbit->Pitch = -20.0f;

	// The wheel's range comes from the subject rather than from constants, so a 5cm bolt and a 3m
	// crate both zoom usefully instead of one of them reaching its limit immediately.
	Orbit->MinRadius = FMath::Max(static_cast<float>(Bounds.SphereRadius) * 0.35f, 5.0f);
	Orbit->MaxRadius = FMath::Max(static_cast<float>(Bounds.SphereRadius) * 12.0f, 500.0f);

	Touch();
	ApplyOrbit();
}

void FMeshDefPreviewClient::ApplyOrbit()
{
	// Pitch stops a tenth of a degree short of the poles. At exactly ±90 the look-at rotation has no
	// defined roll and the view snaps a half turn, which reads as the camera glitching; a tenth of a
	// degree is invisible and every direction around the sphere is still reachable.
	Orbit->Pitch  = FMath::Clamp(Orbit->Pitch, -89.9f, 89.9f);
	Orbit->Yaw    = FMath::Fmod(Orbit->Yaw, 360.0f);
	Orbit->Radius = FMath::Clamp(Orbit->Radius, Orbit->MinRadius, Orbit->MaxRadius);

	const FRotator Rotation(Orbit->Pitch, Orbit->Yaw, 0.0f);

	SetViewRotation(Rotation);
	SetViewLocation(Orbit->Pivot - Rotation.Vector() * Orbit->Radius);

	// Clamping above does not count as a change - only input does - so this is set rather than
	// compared, and a shared viewport is not woken every frame by its neighbour's rounding.
	AppliedSerial = Orbit->Serial;

	Invalidate();
}

void FMeshDefPreviewClient::ApplyViewMode(EViewModeIndex Mode, FName InBufferMode)
{
	// Cleared when the mode is not a buffer visualisation, or the engine keeps drawing the last
	// channel somebody chose behind a menu that says "Lit".
	CurrentBufferVisualizationMode = (Mode == VMI_VisualizeBuffer) ? InBufferMode : NAME_None;

	SetViewMode(Mode);
	Invalidate();
}

bool FMeshDefPreviewClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	const FKey Key = EventArgs.Key;

	// The fly bindings, refused rather than remapped: somebody who presses W expects to move, and
	// nothing happening is a clearer answer than the camera doing something else.
	static const TSet<FKey> Refused = {
		EKeys::W, EKeys::A, EKeys::S, EKeys::D,
		EKeys::Q, EKeys::E, EKeys::Z, EKeys::C,
		EKeys::Up, EKeys::Down, EKeys::Left, EKeys::Right,
		EKeys::MiddleMouseButton,
	};

	if (Refused.Contains(Key))
	{
		return true;
	}

	// Mouse buttons still reach the base class, because that is what captures the cursor and hides
	// it during a drag. What they would *do* to the camera is intercepted in InputAxis instead.
	return FEditorViewportClient::InputKey(EventArgs);
}

bool FMeshDefPreviewClient::InputAxis(const FInputKeyEventArgs& Args)
{
	FViewport* Vp = Args.Viewport;

	if (Vp == nullptr)
	{
		return false;
	}

	const FKey Key = Args.Key;
	const float Delta = Args.AmountDepressed;

	if (Key == EKeys::MouseWheelAxis)
	{
		// The only thing that zooms. Multiplicative, so a click near the subject moves less than a
		// click far away and the last few centimetres do not fly past it.
		Orbit->Radius *= FMath::Pow(0.9f, Delta);
		Touch();
		ApplyOrbit();
		return true;
	}

	if (Key == EKeys::MouseX || Key == EKeys::MouseY)
	{
		// Left drags the camera around the sphere; right drags it in and out. Two buttons, two
		// things, and neither can move the subject off centre - the camera is always computed from
		// the pivot, so there is nothing for a drag to displace.
		if (Vp->KeyState(EKeys::LeftMouseButton))
		{
			const float Speed = 0.35f;

			if (Key == EKeys::MouseX)
			{
				Orbit->Yaw += Delta * Speed;
			}
			else
			{
				Orbit->Pitch -= Delta * Speed;
			}

			Touch();
			ApplyOrbit();
		}
		else if (Vp->KeyState(EKeys::RightMouseButton) && Key == EKeys::MouseY)
		{
			// Vertical only. A right drag that also zoomed sideways would make the distance depend
			// on the path taken to get there rather than on how far the hand moved.
			Orbit->Radius *= FMath::Pow(1.01f, Delta);
			Touch();
			ApplyOrbit();
		}

		// Swallowed either way, so nothing the base class does can move the camera off the subject.
		return true;
	}

	return FEditorViewportClient::InputAxis(Args);
}

// -------------------------------------------------------------------------------------------------
// Viewport widget
// -------------------------------------------------------------------------------------------------

void SMeshDefPreviewViewport::Construct(const FArguments& InArgs)
{
	PreviewScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());

	Component = NewObject<UStaticMeshComponent>();
	Component->bSelectable = false;
	PreviewScene->AddComponent(Component, FTransform::Identity);

	// Registered beside it and empty until a skinned take is chosen. See the declaration for why
	// both exist from the start rather than being swapped in.
	SkeletalComponent = NewObject<USkeletalMeshComponent>();
	SkeletalComponent->bSelectable = false;

	// The reference pose, and nothing animating it. What a garment was wrapped around is the
	// character in its reference pose, so anything else would be showing the shirt against a body
	// it was never fitted to.
	SkeletalComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	SkeletalComponent->SetUpdateAnimationInEditor(false);
	PreviewScene->AddComponent(SkeletalComponent, FTransform::Identity);

	SetFloorVisible(UMeshForgeEditorSettings::Get()->bShowPreviewFloor);

	SEditorViewport::Construct(SEditorViewport::FArguments());
}

void SMeshDefPreviewViewport::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SEditorViewport::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (!bFloorVisible && PreviewScene.IsValid() && PreviewScene->GetFloorVisibility())
	{
		SetFloorVisible(false);
	}
}

void SMeshDefPreviewViewport::SetFloorVisible(bool bVisible)
{
	bFloorVisible = bVisible;
	if (PreviewScene.IsValid())
	{
		// Direct: otherwise the scene writes the choice into the shared asset viewer profile, and the floor
		// disappears from every Static Mesh editor too.
		PreviewScene->SetFloorVisibility(bVisible, /*bDirect*/ true);
	}

	// This viewport draws on demand, so a change nobody asks it to redraw stays off screen: the floor was
	// already hidden while the last picture, drawn with it, stayed up until the mouse next moved over it.
	if (Client.IsValid())
	{
		Client->Invalidate();
	}
}

SMeshDefPreviewViewport::~SMeshDefPreviewViewport()
{
	if (Client.IsValid())
	{
		Client->Viewport = nullptr;
	}
}

TSharedRef<FEditorViewportClient> SMeshDefPreviewViewport::MakeEditorViewportClient()
{
	Client = MakeShared<FMeshDefPreviewClient>(*PreviewScene, SharedThis(this));
	return Client.ToSharedRef();
}

void SMeshDefPreviewViewport::SetMesh(UObject* Mesh, bool bFrame)
{
	if (!Component || !SkeletalComponent)
	{
		return;
	}

	UStaticMesh* Static     = Cast<UStaticMesh>(Mesh);
	USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh);

	// Anything that is neither clears both, rather than leaving whichever was set last on screen
	// under a label that now names something else.
	const bool bSame = (Component->GetStaticMesh() == Static)
		&& (SkeletalComponent->GetSkeletalMeshAsset() == Skeletal);

	if (bSame)
	{
		return;
	}

	Component->SetStaticMesh(Static);
	Component->MarkRenderStateDirty();

	SkeletalComponent->SetSkeletalMeshAsset(Skeletal);
	SkeletalComponent->MarkRenderStateDirty();

	// Placed again, because the component now drawing may never have been given the transform the
	// other one was placed with.
	SetSubjectTransform(SubjectTransform);

	if (bFrame && Client.IsValid())
	{
		Client->FrameBounds(GetVisibleBounds());
	}
}

void SMeshDefPreviewViewport::SetSubjectTransform(const FTransform& Transform)
{
	SubjectTransform = Transform;

	const auto Place = [&Transform](USceneComponent* Target)
	{
		if (Target == nullptr)
		{
			return;
		}

		Target->SetWorldTransform(Transform);
		Target->UpdateBounds();
		Target->MarkRenderStateDirty();
	};

	Place(Component);
	Place(SkeletalComponent);

	// Companions are deliberately not placed with it. They stand at their own origin at their own
	// size, and their being here is what stops the subject being moved at all - see SetCompanions.
}

UObject* SMeshDefPreviewViewport::GetMesh() const
{
	if (Component && Component->GetStaticMesh() != nullptr)
	{
		return Component->GetStaticMesh();
	}

	if (SkeletalComponent && SkeletalComponent->GetSkeletalMeshAsset() != nullptr)
	{
		return SkeletalComponent->GetSkeletalMeshAsset();
	}

	return nullptr;
}

UMeshComponent* SMeshDefPreviewViewport::ActiveComponent() const
{
	if (Component && Component->GetStaticMesh() != nullptr)
	{
		return Component;
	}

	if (SkeletalComponent && SkeletalComponent->GetSkeletalMeshAsset() != nullptr)
	{
		return SkeletalComponent;
	}

	return nullptr;
}

UMeshComponent* SMeshDefPreviewViewport::GetMeshComponent() const
{
	return ActiveComponent();
}

void SMeshDefPreviewViewport::SetCompanions(FName ExtensionId, const TArray<USceneComponent*>& Components)
{
	if (!PreviewScene.IsValid())
	{
		return;
	}

	if (TArray<TObjectPtr<USceneComponent>>* Existing = Companions.Find(ExtensionId))
	{
		for (const TObjectPtr<USceneComponent>& Old : *Existing)
		{
			if (Old != nullptr)
			{
				PreviewScene->RemoveComponent(Old);
			}
		}
	}

	Companions.Remove(ExtensionId);

	if (Components.Num() > 0)
	{
		TArray<TObjectPtr<USceneComponent>>& Added = Companions.Add(ExtensionId);

		for (USceneComponent* New : Components)
		{
			if (New == nullptr)
			{
				continue;
			}

			// Only a primitive has the flag, and only a primitive would be clickable anyway.
			if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(New))
			{
				Primitive->bSelectable = false;
			}

			// At identity, never at SubjectTransform: a companion is the fixed thing in the picture.
			PreviewScene->AddComponent(New, FTransform::Identity);
			Added.Add(New);
		}
	}

	if (Client.IsValid())
	{
		Client->Invalidate();
	}
}

void SMeshDefPreviewViewport::ClearCompanions()
{
	TArray<FName> Ids;
	Companions.GetKeys(Ids);

	for (const FName Id : Ids)
	{
		SetCompanions(Id, TArray<USceneComponent*>());
	}
}

FBoxSphereBounds SMeshDefPreviewViewport::GetVisibleBounds() const
{
	TOptional<FBoxSphereBounds> Total;

	const auto Include = [&Total](const USceneComponent* Part)
	{
		const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Part);

		if (Primitive == nullptr || Primitive->Bounds.SphereRadius <= KINDA_SMALL_NUMBER)
		{
			return;
		}

		Total = Total.IsSet() ? (Total.GetValue() + Primitive->Bounds) : Primitive->Bounds;
	};

	Include(ActiveComponent());

	for (const TPair<FName, TArray<TObjectPtr<USceneComponent>>>& Set : Companions)
	{
		for (const TObjectPtr<USceneComponent>& Companion : Set.Value)
		{
			Include(Companion);
		}
	}

	return Total.IsSet()
		? Total.GetValue()
		: FBoxSphereBounds(FVector::ZeroVector, FVector::ZeroVector, 0.0f);
}

FText SMeshDefPreviewViewport::DescribeMesh(bool bWithPivot) const
{
	UObject* Mesh = GetMesh();
	if (Mesh == nullptr)
	{
		return FText::GetEmpty();
	}

	// Counted from the authored data rather than LOD 0's render data. Under Nanite the render data
	// reports the *fallback* mesh, which is smaller by an order of magnitude and is not what the
	// asset contains - a number that has already been reported wrongly once in this plugin.
	int32 Triangles = 0;
	int32 Vertices  = 0;
	int32 Materials = 0;
	FText Suffix    = FText::GetEmpty();

	FBoxSphereBounds Bounds(FVector::ZeroVector, FVector::ZeroVector, 0.0f);

	if (UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
	{
		if (const FMeshDescription* Description = Static->GetMeshDescription(0))
		{
			Triangles = Description->Triangles().Num();
			Vertices  = Description->Vertices().Num();
		}

		Materials = Static->GetStaticMaterials().Num();
		Bounds    = Static->GetBounds();

		if (Static->IsNaniteEnabled())
		{
			Suffix = LOCTEXT("NaniteSuffix", "  ·  Nanite");
		}
	}
	else if (USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
	{
		if (const FSkeletalMeshModel* Model = Skeletal->GetImportedModel())
		{
			if (Model->LODModels.Num() > 0)
			{
				const FSkeletalMeshLODModel& Lod = Model->LODModels[0];

				Vertices = static_cast<int32>(Lod.NumVertices);

				for (const FSkelMeshSection& Section : Lod.Sections)
				{
					Triangles += Section.NumTriangles;
				}
			}
		}

		Materials = Skeletal->GetMaterials().Num();
		Bounds    = Skeletal->GetBounds();

		// The number that says this is a skinned take at all, and the one somebody checks after a
		// bone profile has folded weights away.
		Suffix = FText::Format(LOCTEXT("BonesSuffix", "  ·  {0} bones"),
			FText::AsNumber(Skeletal->GetRefSkeleton().GetNum()));
	}

	const FVector Size = Bounds.BoxExtent * 2.0;

	// Where the mesh sits, not only how big it is. In true space that is the number that answers
	// "why is it over there" - a garment reported as the right size while it hangs a foot off the
	// body is a strip that has said nothing useful.
	if (bWithPivot)
	{
		Suffix = FText::Format(LOCTEXT("PivotSuffix", "{0}  ·  centre {1}, {2}, {3} cm"),
			Suffix,
			FText::AsNumber(FMath::RoundToInt(Bounds.Origin.X)),
			FText::AsNumber(FMath::RoundToInt(Bounds.Origin.Y)),
			FText::AsNumber(FMath::RoundToInt(Bounds.Origin.Z)));
	}

	return FText::Format(
		LOCTEXT("MeshStats", "{0} triangles  ·  {1} vertices  ·  {2} materials  ·  {3} × {4} × {5} cm{6}"),
		FText::AsNumber(Triangles),
		FText::AsNumber(Vertices),
		FText::AsNumber(Materials),
		FText::AsNumber(FMath::RoundToInt(Size.X)),
		FText::AsNumber(FMath::RoundToInt(Size.Y)),
		FText::AsNumber(FMath::RoundToInt(Size.Z)),
		Suffix);
}

void SMeshDefPreviewViewport::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Component);
	Collector.AddReferencedObject(SkeletalComponent);

	for (TPair<FName, TArray<TObjectPtr<USceneComponent>>>& Set : Companions)
	{
		for (TObjectPtr<USceneComponent>& Companion : Set.Value)
		{
			Collector.AddReferencedObject(Companion);
		}
	}
}

// -------------------------------------------------------------------------------------------------
// The tab
// -------------------------------------------------------------------------------------------------

void SMeshDefPreview::Construct(const FArguments& InArgs)
{
	Definition = InArgs._Definition;

	// Made before the bar, so their controls exist by the time there is a slot to put them in.
	// Each is handed this widget as its host and keeps it for life; nothing here ever reads what
	// an extension built, which is what lets an add-on own its control outright.
	for (const FMeshDefPreviewExtensionType& Type : FMeshDefPreviewExtensions::Get())
	{
		Extensions.Add(Type.Make(SharedThis(this)));
	}

	TSharedPtr<SHorizontalBox> ExtensionBar;

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(8.0f, 4.0f))
			.Visibility(this, &SMeshDefPreview::ViewportVisibility)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.ToolTipText(LOCTEXT("CompareTip",
						"Show two meshes at once, split by a seam you can drag. Both are drawn from "
						"the same camera and framed together, so the only difference you see is the "
						"difference between them."))
					.IsChecked_Lambda([this]()
					{
						return bCompare ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
					{
						SetCompare(State == ECheckBoxState::Checked);
					})
					[
						SNew(STextBlock).Text(LOCTEXT("CompareLabel", "Compare A/B"))
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					.Padding(10.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.Visibility(this, &SMeshDefPreview::CompareOnlyVisibility)
					.ToolTipText(this, &SMeshDefPreview::AlignTooltip)

					// Off the table while something is standing beside the subject. Align exists to
					// throw away size and position; those are the two things a character is here to
					// check. A greyed box that says why beats one that quietly answers the wrong
					// question.
					.IsEnabled_Lambda([this]() { return !IsTrueSpace(); })
					.IsChecked_Lambda([this]()
					{
						return bAlign ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
					{
						bAlign = (State == ECheckBoxState::Checked);
						ApplyMeshes();
					})
					[
						SNew(STextBlock).Text(LOCTEXT("AlignLabel", "Align"))
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					.Padding(12.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SideA", "A"))
					.Visibility(this, &SMeshDefPreview::CompareOnlyVisibility)
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.MaxDesiredWidth(220.0f)
					[
						SNew(SComboButton)
						.ContentPadding(FMargin(6.0f, 2.0f))
						.ToolTipText(LOCTEXT("ChooseMeshTip",
							"Which mesh to look at. Every take this definition has imported is here, "
							"not only the last one."))
						.ButtonContent()
						[
							SNew(STextBlock)
							.Text(this, &SMeshDefPreview::ChoiceLabel, false)
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						]
						.OnGetMenuContent_Lambda([this]() { return BuildChoiceMenu(false); })
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					.Padding(12.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SideB", "B"))
					.Visibility(this, &SMeshDefPreview::CompareOnlyVisibility)
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox)
					.MaxDesiredWidth(220.0f)
					.Visibility(this, &SMeshDefPreview::CompareOnlyVisibility)
					[
						SNew(SComboButton)
						.ContentPadding(FMargin(6.0f, 2.0f))
						.ButtonContent()
						[
							SNew(STextBlock)
							.Text(this, &SMeshDefPreview::ChoiceLabel, true)
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						]
						.OnGetMenuContent_Lambda([this]() { return BuildChoiceMenu(true); })
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("SwapSides", "Swap"))
					.ToolTipText(LOCTEXT("SwapTip",
						"Put A on the right and B on the left. Worth doing once - the side a mesh is "
						"on changes which one looks better."))
					.Visibility(this, &SMeshDefPreview::CompareOnlyVisibility)
					.OnClicked_Lambda([this]() { Swap(); return FReply::Handled(); })
				]

				// Whatever the installed add-ons put here. Beside the take picker rather than over
				// with the floor and the view mode, because these choose what is on screen and
				// those two choose how it is drawn.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SAssignNew(ExtensionBar, SHorizontalBox)
				]

				+ SHorizontalBox::Slot().FillWidth(1.0f)

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					SNew(SCheckBox)
					.ToolTipText(LOCTEXT("FloorTip",
						"Show the floor. Off by default: most generated meshes keep their pivot at their "
						"centre, so the floor cuts through the middle of them. Remembered for next time."))
					.IsChecked_Lambda([]()
					{
						return UMeshForgeEditorSettings::Get()->bShowPreviewFloor ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
					{
						UMeshForgeEditorSettings* Settings = UMeshForgeEditorSettings::Get();
						Settings->bShowPreviewFloor = (State == ECheckBoxState::Checked);
						Settings->SaveConfig();
						ApplyFloor();
					})
					[
						SNew(STextBlock).Text(LOCTEXT("FloorLabel", "Floor"))
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SComboButton)
					.ContentPadding(FMargin(6.0f, 2.0f))
					.ToolTipText(LOCTEXT("ViewModeTip",
						"How the mesh is drawn. Applies to the whole viewer - comparing a lit mesh "
						"against a wireframe one answers nothing."))
					.ButtonContent()
					[
						SNew(STextBlock).Text(this, &SMeshDefPreview::ViewModeLabel)
					]
					.OnGetMenuContent_Lambda([this]() { return BuildViewModeMenu(); })
				]
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SComboButton)
			.Visibility_Lambda([this]() { const UMeshDef* Def = Definition.Get(); return Def && !Def->PostOutputs.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
			.ButtonContent()[ SNew(STextBlock).Text(LOCTEXT("PostOutputs", "Open post-processing output")) ]
			.OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
			{
				FMenuBuilder Menu(true, nullptr);
				const UMeshDef* Def = Definition.Get();
				if (Def)
				{
					for (int32 Index = Def->PostOutputs.Num() - 1; Index >= 0; --Index)
					{
						const FMeshPostOutput& Output = Def->PostOutputs[Index];
						const TSoftObjectPtr<UObject> Asset = Output.Asset;
						Menu.AddMenuEntry(FText::FromString(Output.Step + TEXT(" - ") + Output.CreatedUtc.ToString()),
							FText::FromString(Asset.ToString()), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([Asset]()
							{
								if (UObject* Object = Asset.LoadSynchronous())
									GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Object);
							})));
					}
				}
				return Menu.MakeWidget();
			})
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SOverlay)

			+ SOverlay::Slot()
			[
				SNew(SBox)
				.Visibility(this, &SMeshDefPreview::ViewportVisibility)
				[
					SAssignNew(Wipe, SMeshCompareWipe)
					.ASide()
					[
						SAssignNew(Viewport, SMeshDefPreviewViewport)
					]
					.BSide()
					[
						SAssignNew(ViewportB, SMeshDefPreviewViewport)
					]
				]
			]

			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Visibility(this, &SMeshDefPreview::EmptyVisibility)
				.Text(this, &SMeshDefPreview::EmptyMessage)
				.Justification(ETextJustify::Center)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(8.0f, 4.0f))
			.Visibility(this, &SMeshDefPreview::ViewportVisibility)
			[
				SNew(STextBlock)
				.Text(this, &SMeshDefPreview::StatsText)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]
	];

	// **One camera, shared rather than synchronised.** Two clients holding the same numbers cannot
	// drift; two clients copying each other's numbers eventually do, and the wipe then compares the
	// camera instead of the meshes.
	if (Viewport.IsValid() && ViewportB.IsValid())
	{
		if (const TSharedPtr<FMeshDefPreviewClient> A = Viewport->GetClient())
		{
			if (const TSharedPtr<FMeshDefPreviewClient> B = ViewportB->GetClient())
			{
				B->ShareOrbit(A->GetOrbit());
			}
		}
	}

	if (ExtensionBar.IsValid())
	{
		for (const TSharedRef<IMeshDefPreviewExtension>& Extension : Extensions)
		{
			if (const TSharedPtr<SWidget> Control = Extension->MakeControl())
			{
				ExtensionBar->AddSlot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(12.0f, 0.0f, 0.0f, 0.0f)
					[
						Control.ToSharedRef()
					];
			}
		}
	}

	Refresh();
}

void SMeshDefPreview::Refresh()
{
	GatherChoices();
	ApplyMeshes();
	ApplyViewMode();
}

void SMeshDefPreview::GatherChoices()
{
	Choices.Reset();

	const UMeshDef* Def = Definition.Get();

	if (Def == nullptr)
	{
		return;
	}

	const UMeshForgeSettings* Settings = GetDefault<UMeshForgeSettings>();
	const FString Root = Settings ? Settings->GetGeneratedFolder(Def->GetName()) : FString();

	// The take records, so a mesh can be named after the take that made it rather than after the
	// folder it landed in. A take id is a provider task id - it says nothing to anybody - while
	// "Meshy - meshy-7, 00:44" is the same row somebody is reading in the Takes tab.
	TMap<FString, FForgeTakeRecord> Records;

	for (const FForgeTakeRecord& Record : FForgeLibrary::ListTakes(TEXT("MeshForge"), Def->GetName()))
	{
		Records.Add(Record.TakeId, Record);
	}

	if (!Root.IsEmpty())
	{
		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		FARFilter Filter;
		Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());

		// The skinned takes too. A garment stops being a static mesh at its skinning step, and a
		// list that ended there could show every take of a shirt except the finished one.
		Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(FName(*Root));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Found;
		Registry.GetAssets(Filter, Found);

		for (const FAssetData& Asset : Found)
		{
			FMeshChoice Choice;
			Choice.Mesh = TSoftObjectPtr<UObject>(Asset.GetSoftObjectPath());

			// <root>/Mesh/<take id>/... or <root>/Post/<take id>/...
			FString Relative = Asset.PackagePath.ToString();
			Relative.RemoveFromStart(Root);
			Relative.RemoveFromStart(TEXT("/"));

			TArray<FString> Parts;
			Relative.ParseIntoArray(Parts, TEXT("/"));

			const FString Stage  = Parts.Num() > 0 ? Parts[0] : FString();
			const FString TakeId = Parts.Num() > 1 ? Parts[1] : FString();

			const FForgeTakeRecord* Record = Records.Find(TakeId);

			// A short id is still worth showing: it is what the take folder on disk is called, and
			// what somebody looking for the artifact by hand has to match.
			const FText Short = FText::FromString(TakeId.Left(8));

			if (Record != nullptr)
			{
				const FText Maker = Record->ModelId.IsEmpty()
					? FText::FromString(Record->ProviderId)
					: FText::Format(LOCTEXT("TakeMaker", "{0} - {1}"),
						FText::FromString(Record->ProviderId), FText::FromString(Record->ModelId));

				Choice.Label = FText::Format(LOCTEXT("TakeChoice", "{0}  -  {1}"),
					Maker,
					(Record->FinishedUtc == FDateTime::MinValue())
						? Short : FText::AsDateTime(Record->FinishedUtc));

				Choice.Modified = Record->FinishedUtc;
			}
			else
			{
				Choice.Label = FText::Format(LOCTEXT("TakeChoiceBare", "take {0}"), Short);
			}

			// A post-processed take is a different thing from the take it was made from, and the two
			// sit side by side in the list.
			if (Stage == TEXT("Post"))
			{
				Choice.Label = FText::Format(LOCTEXT("PostChoice", "post-processed  -  {0}"), Choice.Label);
			}

			Choice.Detail = FText::Format(LOCTEXT("TakeChoiceDetail", "take {0}\n{1}"),
				FText::FromString(TakeId), FText::FromString(Asset.GetSoftObjectPath().ToString()));

			if (Choice.Modified == FDateTime::MinValue())
			{
				const FString File = FPackageName::LongPackageNameToFilename(
					Asset.PackageName.ToString(), FPackageName::GetAssetPackageExtension());

				Choice.Modified = IFileManager::Get().GetTimeStamp(*File);
			}

			Choices.Add(MoveTemp(Choice));
		}
	}

	// Newest first, by when the package was written. Not by name: a take id is a provider task id
	// or a GUID, so sorting on it produces an order that looks meaningful and means nothing.
	Choices.Sort([](const FMeshChoice& A, const FMeshChoice& B)
	{
		return A.Modified > B.Modified;
	});

	// A mesh somebody supplied by hand belongs in the list too - it is the thing a post-processed
	// take should be compared against.
	if (!Def->SourceMesh.IsNull())
	{
		FMeshChoice Supplied;
		Supplied.Mesh   = TSoftObjectPtr<UObject>(Def->SourceMesh.ToSoftObjectPath());
		Supplied.Label  = LOCTEXT("SuppliedChoice", "supplied mesh");
		Supplied.Detail = FText::FromString(Def->SourceMesh.ToString());

		Choices.Insert(MoveTemp(Supplied), 0);
	}

	// And whatever was imported last, if it somehow lives outside the generated folder - an older
	// definition, or one somebody pointed elsewhere. Both results: a garment chain leaves a wrapped
	// static mesh and a skinned one, and they are two different things to look at.
	const auto AddImported = [this](const FSoftObjectPath& Path, const FText& Label)
	{
		if (Path.IsNull()
			|| Choices.ContainsByPredicate([&Path](const FMeshChoice& Choice)
				{ return Choice.Mesh.ToSoftObjectPath() == Path; }))
		{
			return;
		}

		FMeshChoice Current;
		Current.Mesh   = TSoftObjectPtr<UObject>(Path);
		Current.Label  = Label;
		Current.Detail = FText::FromString(Path.ToString());

		Choices.Insert(MoveTemp(Current), 0);
	};

	AddImported(Def->ImportedMesh.ToSoftObjectPath(), LOCTEXT("CurrentChoice", "current import"));
	AddImported(Def->ImportedSkeletalMesh.ToSoftObjectPath(), LOCTEXT("CurrentSkinnedChoice", "current import - skinned"));

	// A choice that has been deleted from the project since it was made is not a choice any more.
	if (!ChosenA.IsNull()
		&& !Choices.ContainsByPredicate([this](const FMeshChoice& C) { return C.Mesh == ChosenA; }))
	{
		ChosenA.Reset();
	}

	if (!ChosenB.IsNull()
		&& !Choices.ContainsByPredicate([this](const FMeshChoice& C) { return C.Mesh == ChosenB; }))
	{
		ChosenB.Reset();
	}
}

TSharedRef<SWidget> SMeshDefPreview::BuildChoiceMenu(bool bSideB)
{
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);

	const UMeshDef* Def = Definition.Get();
	const TSoftObjectPtr<UObject> Current = Chosen(bSideB);

	if (Choices.Num() == 0)
	{
		Menu.AddWidget(
			SNew(STextBlock)
			.Text(LOCTEXT("NoChoices", "Nothing imported yet."))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground()),
			FText::GetEmpty());

		return Menu.MakeWidget();
	}

	for (const FMeshChoice& Choice : Choices)
	{
		const TSoftObjectPtr<UObject> Mesh = Choice.Mesh;
		const bool bIsCurrentImport = Def && (Mesh == Def->ImportedMesh);

		Menu.AddMenuEntry(
			bIsCurrentImport
				? FText::Format(LOCTEXT("ChoiceCurrent", "{0}  (current)"), Choice.Label)
				: Choice.Label,
			Choice.Detail,
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SMeshDefPreview::Choose, bSideB, Mesh),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([Mesh, Current]() { return Mesh == Current; })),
			NAME_None, EUserInterfaceActionType::Check);
	}

	return Menu.MakeWidget();
}

TSharedRef<SWidget> SMeshDefPreview::BuildViewModeMenu()
{
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);

	for (const MeshDefPreviewPrivate::FViewModeEntry& Entry : MeshDefPreviewPrivate::ViewModes)
	{
		const EViewModeIndex Mode = Entry.Mode;
		const FName Buffer = Entry.Buffer ? FName(Entry.Buffer) : NAME_None;

		Menu.AddMenuEntry(
			FText::FromString(Entry.Label),
			FText::FromString(Entry.Tip),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([this, Mode, Buffer]()
				{
					ViewMode   = Mode;
					BufferMode = Buffer;
					ApplyViewMode();
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([this, Mode, Buffer]()
				{
					return ViewMode == Mode && BufferMode == Buffer;
				})),
			NAME_None, EUserInterfaceActionType::RadioButton);
	}

	return Menu.MakeWidget();
}

TSoftObjectPtr<UObject> SMeshDefPreview::Chosen(bool bSideB) const
{
	if (bSideB)
	{
		return ChosenB;
	}

	// A falls back to whatever the definition last imported, so the tab opens on the thing somebody
	// most likely came to look at without anybody having to choose it.
	if (!ChosenA.IsNull())
	{
		return ChosenA;
	}

	const UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return TSoftObjectPtr<UObject>();
	}

	// The skinned result first, the same order the Content Browser tile uses: where a chain made
	// both, the skinned one is the later of the two and the one the chain was run for.
	if (!Def->ImportedSkeletalMesh.IsNull())
	{
		return TSoftObjectPtr<UObject>(Def->ImportedSkeletalMesh.ToSoftObjectPath());
	}

	return TSoftObjectPtr<UObject>(Def->ImportedMesh.ToSoftObjectPath());
}

void SMeshDefPreview::Choose(bool bSideB, TSoftObjectPtr<UObject> Mesh)
{
	(bSideB ? ChosenB : ChosenA) = Mesh;
	ApplyMeshes();
}

void SMeshDefPreview::SetCompare(bool bOn)
{
	bCompare = bOn;

	if (Wipe.IsValid())
	{
		Wipe->SetCompare(bOn);
	}

	// Something has to be on the other side, and an empty half is a worse first impression than a
	// guess: the next take down the list, or the same mesh if there is only one.
	if (bOn && ChosenB.IsNull())
	{
		const TSoftObjectPtr<UObject> A = Chosen(false);

		for (const FMeshChoice& Choice : Choices)
		{
			if (Choice.Mesh != A)
			{
				ChosenB = Choice.Mesh;
				break;
			}
		}

		if (ChosenB.IsNull())
		{
			ChosenB = A;
		}
	}

	ApplyMeshes();
	ApplyViewMode();
}

void SMeshDefPreview::Swap()
{
	const TSoftObjectPtr<UObject> A = Chosen(false);

	ChosenA = ChosenB;
	ChosenB = A;

	ApplyMeshes();
}

void SMeshDefPreview::ApplyMeshes()
{
	if (!Viewport.IsValid())
	{
		return;
	}

	UObject* A = Chosen(false).LoadSynchronous();
	UObject* B = bCompare ? Chosen(true).LoadSynchronous() : nullptr;

	// Never framed by SetMesh. Two meshes of different sizes have to share one framing or the
	// comparison is between two distances from the camera, and whichever side was set last would
	// decide where the shared camera sits.
	Viewport->SetMesh(A, /*bFrame*/ false);

	if (ViewportB.IsValid())
	{
		ViewportB->SetMesh(B, /*bFrame*/ false);
	}

	// **Only when the choice changes.** Refresh runs whenever any job in the editor changes state,
	// and a viewer that re-framed on each of those would snatch the camera back to
	// three-quarters-from-above while somebody was leaning into a seam.
	const bool bChanged = (FramedA.Get() != A)
		|| (FramedB.Get() != B)
		|| (bFramedCompare != bCompare)
		|| (bFramedAlign != bAlign);

	// The second viewport stops being a side when the seam is put away, and nothing will ask an
	// extension for its companions again - so they come out here rather than being left to reappear
	// the next time two takes are compared.
	if (!bCompare && ViewportB.IsValid())
	{
		ViewportB->ClearCompanions();
	}

	// Told before anything is placed: whether an extension puts a character in decides which space
	// both subjects are placed in, so the answer has to be in before the placing starts. Each call
	// to SetCompanions places what is there at that moment; ApplySpace below is the last word.
	for (const TSharedRef<IMeshDefPreviewExtension>& Extension : Extensions)
	{
		Extension->OnPreviewChanged();
	}

	ApplySpace();

	if (!bChanged)
	{
		return;
	}

	FramedA        = A;
	FramedB        = B;
	bFramedCompare = bCompare;
	bFramedAlign   = bAlign;

	if (A == nullptr && B == nullptr)
	{
		return;
	}

	Reframe();
}

bool SMeshDefPreview::IsTrueSpace() const
{
	if (Viewport.IsValid() && Viewport->HasCompanions())
	{
		return true;
	}

	return bCompare && ViewportB.IsValid() && ViewportB->HasCompanions();
}

void SMeshDefPreview::ApplySpace()
{
	if (!Viewport.IsValid())
	{
		return;
	}

	UObject* A = Chosen(false).LoadSynchronous();
	UObject* B = bCompare ? Chosen(true).LoadSynchronous() : nullptr;

	AlignScale     = 1.0f;
	AlignRotation  = FRotator::ZeroRotator;
	TrueSpaceRatio = 1.0f;

	const bool bTrueSpace = IsTrueSpace();

	if (bTrueSpace)
	{
		if (A != nullptr && B != nullptr)
		{
			const float RadiusA = MeshDefPreviewPrivate::Measure(A).Radius;
			const float RadiusB = MeshDefPreviewPrivate::Measure(B).Radius;

			TrueSpaceRatio = (RadiusA > KINDA_SMALL_NUMBER) ? (RadiusB / RadiusA) : 1.0f;
		}

		// Nothing is done to anything. Both meshes and everything beside them stand where their own
		// data puts them, at their own size - the picture a level would give with all of them
		// dropped at 0, 0, 0. A garment that was wrapped around this character lands on it exactly;
		// one that was not hangs wrong, and that is the answer rather than a fault in the viewer.
		Viewport->SetSubjectTransform(FTransform::Identity);

		if (ViewportB.IsValid())
		{
			ViewportB->SetSubjectTransform(FTransform::Identity);
		}
	}
	else
	{
		const MeshDefPreviewPrivate::FSubject SubjectA = MeshDefPreviewPrivate::Measure(A);

		// A is never moved except onto the pivot: it is the reference, so "aligned" means one thing
		// rather than depending on which side you happen to be looking at.
		if (A != nullptr)
		{
			Viewport->SetSubjectTransform(FTransform(-SubjectA.Centre));
		}

		if (B != nullptr && ViewportB.IsValid())
		{
			const MeshDefPreviewPrivate::FSubject SubjectB = MeshDefPreviewPrivate::Measure(B);

			if (bAlign && A != nullptr)
			{
				AlignRotation = MeshDefPreviewPrivate::BestRotation(SubjectB, SubjectA);
				AlignScale    = SubjectA.Radius / SubjectB.Radius;

				const FQuat Turn = AlignRotation.Quaternion();

				// Turned about its own centre, then dropped on the pivot A is sitting on.
				FTransform Fit(Turn);
				Fit.SetScale3D(FVector(AlignScale));
				Fit.SetTranslation(-Turn.RotateVector(SubjectB.Centre) * AlignScale);

				ViewportB->SetSubjectTransform(Fit);
			}
			else
			{
				ViewportB->SetSubjectTransform(FTransform(-SubjectB.Centre));
			}
		}
	}

	// The camera follows the space, not every change within it: a character swapped for another
	// leaves the picture the same size and the camera where somebody put it.
	if (bTrueSpace != bFramedTrueSpace)
	{
		bFramedTrueSpace = bTrueSpace;
		Reframe();
	}
}

void SMeshDefPreview::Reframe()
{
	if (!Viewport.IsValid())
	{
		return;
	}

	const TSharedPtr<FMeshDefPreviewClient> Client = Viewport->GetClient();

	if (!Client.IsValid())
	{
		return;
	}

	// The union of what is actually drawn, companions included. Framing the subject alone would
	// put the head and the knees of a character standing behind a shirt off screen - which is the
	// one thing it was put there to show.
	FBoxSphereBounds Visible = Viewport->GetVisibleBounds();

	if (bCompare && ViewportB.IsValid())
	{
		Visible = Visible + ViewportB->GetVisibleBounds();
	}

	if (Visible.SphereRadius <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	Client->FrameBounds(Visible);
}

UObject* SMeshDefPreview::GetSubject(int32 Side) const
{
	if (Side == 0)
	{
		return Viewport.IsValid() ? Viewport->GetMesh() : nullptr;
	}

	return (bCompare && ViewportB.IsValid()) ? ViewportB->GetMesh() : nullptr;
}

void SMeshDefPreview::SetCompanions(FName ExtensionId, int32 Side, const TArray<USceneComponent*>& Components)
{
	const TSharedPtr<SMeshDefPreviewViewport> Target = (Side == 0) ? Viewport : ViewportB;

	if (Target.IsValid())
	{
		Target->SetCompanions(ExtensionId, Components);
	}

	// A companion arriving or leaving changes which space the viewer is in, so both subjects are
	// placed again. This is also the path a person takes when they pick a character from the menu,
	// which happens nowhere near ApplyMeshes.
	ApplySpace();
}

void SMeshDefPreview::Redraw()
{
	if (Viewport.IsValid())
	{
		if (const TSharedPtr<FMeshDefPreviewClient> Client = Viewport->GetClient())
		{
			Client->Invalidate();
		}
	}

	if (ViewportB.IsValid())
	{
		if (const TSharedPtr<FMeshDefPreviewClient> Client = ViewportB->GetClient())
		{
			Client->Invalidate();
		}
	}
}

void SMeshDefPreview::ApplyViewMode()
{
	if (Viewport.IsValid())
	{
		if (const TSharedPtr<FMeshDefPreviewClient> Client = Viewport->GetClient())
		{
			Client->ApplyViewMode(ViewMode, BufferMode);
		}
	}

	if (ViewportB.IsValid())
	{
		if (const TSharedPtr<FMeshDefPreviewClient> Client = ViewportB->GetClient())
		{
			Client->ApplyViewMode(ViewMode, BufferMode);
		}
	}
}

void SMeshDefPreview::ApplyFloor()
{
	const bool bVisible = UMeshForgeEditorSettings::Get()->bShowPreviewFloor;

	if (Viewport.IsValid())
	{
		Viewport->SetFloorVisible(bVisible);
	}

	if (ViewportB.IsValid())
	{
		ViewportB->SetFloorVisible(bVisible);
	}
}

FText SMeshDefPreview::ChoiceLabel(bool bSideB) const
{
	const TSoftObjectPtr<UObject> Mesh = Chosen(bSideB);

	if (Mesh.IsNull())
	{
		return LOCTEXT("ChooseMesh", "Choose...");
	}

	for (const FMeshChoice& Choice : Choices)
	{
		if (Choice.Mesh == Mesh)
		{
			return Choice.Label;
		}
	}

	return FText::FromString(FPackageName::ObjectPathToObjectName(Mesh.ToString()));
}

FText SMeshDefPreview::AlignTooltip() const
{
	if (IsTrueSpace())
	{
		return LOCTEXT("AlignTipTrueSpace",
			"Not while something is standing beside the mesh.\n\n"
			"Align throws away size and position so two takes can be compared on shape alone - and "
			"those are the two things a character is in the picture to check. With one there, "
			"everything is drawn at its own size and its own pivot, exactly as a level would show it "
			"with all of them dropped at 0, 0, 0. A garment that fits, fits; one that does not, "
			"hangs wrong.");
	}

	return LOCTEXT("AlignTip",
		"Fit B onto A: same centre, same size, and turned by whichever quarter turn best "
		"matches its shape to A's.\n\n"
		"Two generators disagree about which way is up, not by nineteen degrees, so a "
		"quarter turn is usually the whole difference. A is never moved, the strip below "
		"says what B was fitted by, and both meshes true dimensions are reported whatever "
		"this is set to.");
}

FText SMeshDefPreview::ViewModeLabel() const
{
	for (const MeshDefPreviewPrivate::FViewModeEntry& Entry : MeshDefPreviewPrivate::ViewModes)
	{
		const FName Buffer = Entry.Buffer ? FName(Entry.Buffer) : NAME_None;

		if (Entry.Mode == ViewMode && Buffer == BufferMode)
		{
			return FText::FromString(Entry.Label);
		}
	}

	return LOCTEXT("ViewModeUnknown", "View");
}

EVisibility SMeshDefPreview::CompareOnlyVisibility() const
{
	return bCompare ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SMeshDefPreview::ViewportVisibility() const
{
	// Anything to look at at all, rather than only the last import - the whole point of the list is
	// that earlier takes are still there.
	return (Choices.Num() > 0) ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SMeshDefPreview::EmptyVisibility() const
{
	return ViewportVisibility() == EVisibility::Visible ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SMeshDefPreview::EmptyMessage() const
{
	const UMeshDef* Def = Definition.Get();
	if (Def == nullptr)
	{
		return FText::GetEmpty();
	}

	// Which stage is missing, rather than "nothing here". A definition spends most of its life
	// without an imported mesh, and a blank viewport that does not say why is a dead end.
	if (Def->Candidates.Num() == 0)
	{
		return LOCTEXT("NoMeshYet",
			"No mesh yet.\n\nRun the Mesh stage to generate one.");
	}

	return LOCTEXT("NotImportedYet",
		"Generated, but not imported.\n\nRun the Import stage to bring it into the project.");
}

FText SMeshDefPreview::StatsText() const
{
	if (!Viewport.IsValid())
	{
		return FText::GetEmpty();
	}

	const bool bTrueSpace = IsTrueSpace();

	const FText A = Viewport->DescribeMesh(bTrueSpace);

	if (!bCompare || !ViewportB.IsValid())
	{
		return A;
	}

	const FText B = ViewportB->DescribeMesh(bTrueSpace);

	// In true space nothing was done to either of them, so the only thing left to say is how they
	// differ - said as a number, because "the second one looks bigger" is not a finding. The ratio
	// was worked out when they were placed; this text is read every frame.
	if (bTrueSpace)
	{
		FNumberFormattingOptions TwoDigits;
		TwoDigits.SetMaximumFractionalDigits(2);

		return FText::Format(
			LOCTEXT("StatsABTrue", "A  {0}\nB  {1}\ntrue space - nothing moved or scaled. B is {2}x A."),
			A, B, FText::AsNumber(TrueSpaceRatio, &TwoDigits));
	}

	// **What the fit did, said rather than left to be noticed.** A viewer that silently resizes and
	// turns one of two things being compared is a viewer that cannot be trusted for the comparison
	// it exists to make.
	if (!bAlign
		|| (FMath::IsNearlyEqual(AlignScale, 1.0f, 0.01f) && AlignRotation.IsNearlyZero()))
	{
		return FText::Format(LOCTEXT("StatsAB", "A  {0}\nB  {1}"), A, B);
	}

	FNumberFormattingOptions Two;
	Two.SetMaximumFractionalDigits(2);

	return FText::Format(
		LOCTEXT("StatsABFitted", "A  {0}\nB  {1}\nfitted to A: scaled x{2}, turned {3}"),
		A, B,
		FText::AsNumber(AlignScale, &Two),
		FText::FromString(AlignRotation.ToCompactString()));
}

#undef LOCTEXT_NAMESPACE

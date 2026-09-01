#include "SMeshDefPreview.h"

#include "MeshDef.h"
#include "ForgeLibrary.h"
#include "MeshForgeSettings.h"
#include "SMeshCompareWipe.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
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

	static FSubject Measure(UStaticMesh* Mesh)
	{
		FSubject Subject;

		if (Mesh == nullptr)
		{
			return Subject;
		}

		const FBoxSphereBounds Bounds = Mesh->GetBounds();

		Subject.Centre = Bounds.Origin;
		Subject.Extent = Bounds.BoxExtent.ComponentMax(FVector(KINDA_SMALL_NUMBER));
		Subject.Radius = FMath::Max(static_cast<float>(Bounds.SphereRadius), KINDA_SMALL_NUMBER);

		const FMeshDescription* Description = Mesh->GetMeshDescription(0);

		if (Description == nullptr)
		{
			return Subject;
		}

		const auto Positions = Description->GetVertexPositions();

		const int32 Count = Description->Vertices().Num();

		if (Count <= 0)
		{
			return Subject;
		}

		// Strided rather than every vertex: the centroid of a fifth of a million points is the same
		// answer as the centroid of a million, and this runs whenever a take is chosen.
		const int32 Stride = FMath::Max(1, Count / SkewSamples);

		FVector Sum = FVector::ZeroVector;
		int32 Taken = 0;

		for (int32 Index = 0; Index < Count; Index += Stride)
		{
			const FVertexID Vertex(Index);

			if (Description->IsVertexValid(Vertex))
			{
				Sum += FVector(Positions[Vertex]);
				++Taken;
			}
		}

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

void FMeshDefPreviewClient::FrameMesh(UStaticMeshComponent* InComponent)
{
	if (InComponent == nullptr || InComponent->GetStaticMesh() == nullptr)
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

	SEditorViewport::Construct(SEditorViewport::FArguments());
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

void SMeshDefPreviewViewport::SetMesh(UStaticMesh* Mesh, bool bFrame)
{
	if (!Component)
	{
		return;
	}

	if (Component->GetStaticMesh() == Mesh)
	{
		return;
	}

	Component->SetStaticMesh(Mesh);
	Component->MarkRenderStateDirty();

	if (bFrame && Client.IsValid())
	{
		Client->FrameMesh(Component);
	}
}

void SMeshDefPreviewViewport::SetSubjectTransform(const FTransform& Transform)
{
	if (Component == nullptr)
	{
		return;
	}

	Component->SetWorldTransform(Transform);
	Component->UpdateBounds();
	Component->MarkRenderStateDirty();
}

UStaticMesh* SMeshDefPreviewViewport::GetMesh() const
{
	return Component ? Component->GetStaticMesh() : nullptr;
}

FText SMeshDefPreviewViewport::DescribeMesh() const
{
	UStaticMesh* Mesh = GetMesh();
	if (Mesh == nullptr)
	{
		return FText::GetEmpty();
	}

	// Counted from the mesh description rather than LOD 0's render data. Under Nanite the render
	// data reports the *fallback* mesh, which is smaller by an order of magnitude and is not what
	// the asset contains - a number that has already been reported wrongly once in this plugin.
	int32 Triangles = 0;
	int32 Vertices  = 0;
	if (const FMeshDescription* Description = Mesh->GetMeshDescription(0))
	{
		Triangles = Description->Triangles().Num();
		Vertices  = Description->Vertices().Num();
	}

	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	const FVector Size = Bounds.BoxExtent * 2.0;

	return FText::Format(
		LOCTEXT("MeshStats", "{0} triangles  ·  {1} vertices  ·  {2} materials  ·  {3} × {4} × {5} cm{6}"),
		FText::AsNumber(Triangles),
		FText::AsNumber(Vertices),
		FText::AsNumber(Mesh->GetStaticMaterials().Num()),
		FText::AsNumber(FMath::RoundToInt(Size.X)),
		FText::AsNumber(FMath::RoundToInt(Size.Y)),
		FText::AsNumber(FMath::RoundToInt(Size.Z)),
		Mesh->IsNaniteEnabled() ? LOCTEXT("NaniteSuffix", "  ·  Nanite") : FText::GetEmpty());
}

void SMeshDefPreviewViewport::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Component);
}

// -------------------------------------------------------------------------------------------------
// The tab
// -------------------------------------------------------------------------------------------------

void SMeshDefPreview::Construct(const FArguments& InArgs)
{
	Definition = InArgs._Definition;

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
					.ToolTipText(LOCTEXT("AlignTip",
						"Fit B onto A: same centre, same size, and turned by whichever quarter turn best "
						"matches its shape to A's.\n\n"
						"Two generators disagree about which way is up, not by nineteen degrees, so a "
						"quarter turn is usually the whole difference. A is never moved, the strip below "
						"says what B was fitted by, and both meshes true dimensions are reported whatever "
						"this is set to."))
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

				+ SHorizontalBox::Slot().FillWidth(1.0f)

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
		Filter.PackagePaths.Add(FName(*Root));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Found;
		Registry.GetAssets(Filter, Found);

		for (const FAssetData& Asset : Found)
		{
			FMeshChoice Choice;
			Choice.Mesh = TSoftObjectPtr<UStaticMesh>(Asset.GetSoftObjectPath());

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
		Supplied.Mesh   = Def->SourceMesh;
		Supplied.Label  = LOCTEXT("SuppliedChoice", "supplied mesh");
		Supplied.Detail = FText::FromString(Def->SourceMesh.ToString());

		Choices.Insert(MoveTemp(Supplied), 0);
	}

	// And whatever was imported last, if it somehow lives outside the generated folder - an older
	// definition, or one somebody pointed elsewhere.
	if (!Def->ImportedMesh.IsNull()
		&& !Choices.ContainsByPredicate([Def](const FMeshChoice& Choice)
			{ return Choice.Mesh == Def->ImportedMesh; }))
	{
		FMeshChoice Current;
		Current.Mesh   = Def->ImportedMesh;
		Current.Label  = LOCTEXT("CurrentChoice", "current import");
		Current.Detail = FText::FromString(Def->ImportedMesh.ToString());

		Choices.Insert(MoveTemp(Current), 0);
	}

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
	const TSoftObjectPtr<UStaticMesh> Current = Chosen(bSideB);

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
		const TSoftObjectPtr<UStaticMesh> Mesh = Choice.Mesh;
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

TSoftObjectPtr<UStaticMesh> SMeshDefPreview::Chosen(bool bSideB) const
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
	return Def ? Def->ImportedMesh : TSoftObjectPtr<UStaticMesh>();
}

void SMeshDefPreview::Choose(bool bSideB, TSoftObjectPtr<UStaticMesh> Mesh)
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
		const TSoftObjectPtr<UStaticMesh> A = Chosen(false);

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
	const TSoftObjectPtr<UStaticMesh> A = Chosen(false);

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

	UStaticMesh* A = Chosen(false).LoadSynchronous();
	UStaticMesh* B = bCompare ? Chosen(true).LoadSynchronous() : nullptr;

	// Never framed by SetMesh. Two meshes of different sizes have to share one framing or the
	// comparison is between two distances from the camera, and whichever side was set last would
	// decide where the shared camera sits.
	Viewport->SetMesh(A, /*bFrame*/ false);

	if (ViewportB.IsValid())
	{
		ViewportB->SetMesh(B, /*bFrame*/ false);
	}

	AlignScale    = 1.0f;
	AlignRotation = FRotator::ZeroRotator;

	// **Only when the choice changes.** Refresh runs whenever any job in the editor changes state,
	// and a viewer that re-framed on each of those would snatch the camera back to
	// three-quarters-from-above while somebody was leaning into a seam.
	const bool bChanged = (FramedA.Get() != A)
		|| (FramedB.Get() != B)
		|| (bFramedCompare != bCompare)
		|| (bFramedAlign != bAlign);

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

	if (!bChanged)
	{
		return;
	}

	FramedA        = A;
	FramedB        = B;
	bFramedCompare = bCompare;
	bFramedAlign   = bAlign;

	const TSharedPtr<FMeshDefPreviewClient> Client = Viewport->GetClient();

	if (!Client.IsValid() || (A == nullptr && B == nullptr))
	{
		return;
	}

	// Framed on the union of what is actually on screen - which, once both are centred on the
	// pivot and B is fitted to A, is a sphere about the origin.
	const float Radius = (A != nullptr)
		? SubjectA.Radius
		: MeshDefPreviewPrivate::Measure(B).Radius;

	float Widest = Radius;

	if (!bAlign && A != nullptr && B != nullptr)
	{
		// Unaligned, the two are their own sizes and the framing has to hold the larger.
		Widest = FMath::Max(Radius, MeshDefPreviewPrivate::Measure(B).Radius);
	}

	Client->FrameBounds(FBoxSphereBounds(FVector::ZeroVector,
		FVector(Widest), Widest));
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

FText SMeshDefPreview::ChoiceLabel(bool bSideB) const
{
	const TSoftObjectPtr<UStaticMesh> Mesh = Chosen(bSideB);

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

	const FText A = Viewport->DescribeMesh();

	if (!bCompare || !ViewportB.IsValid())
	{
		return A;
	}

	const FText B = ViewportB->DescribeMesh();

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

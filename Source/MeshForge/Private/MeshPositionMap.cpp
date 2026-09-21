#include "MeshPositionMap.h"

#include "Misc/FileHelper.h"

bool FMeshPositionMap::ReadFile(const FString& Path, TArray<FVector3f>& OutBefore, TArray<FVector3f>& OutAfter)
{
	TArray<uint8> Bytes;
	if (Path.IsEmpty() || !FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent)
		|| Bytes.Num() < 8 || FMemory::Memcmp(Bytes.GetData(), "GFP1", 4) != 0)
	{
		return false;
	}

	uint32 Count = 0;
	FMemory::Memcpy(&Count, Bytes.GetData() + 4, sizeof(uint32));
	if (Bytes.Num() != 8 + int64(Count) * 2 * 3 * sizeof(float))
	{
		return false;
	}

	// Blender read the glTF as (x, -z, y) of what the exporter wrote, which was (x, z, y) x 0.01 of Unreal's.
	const float* Floats = reinterpret_cast<const float*>(Bytes.GetData() + 8);
	auto Read = [Floats, Count](int32 Block, TArray<FVector3f>& Out)
	{
		Out.SetNumUninitialized(Count);
		const float* Base = Floats + int64(Block) * Count * 3;
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			Out[Index] = FVector3f(Base[Index * 3], -Base[Index * 3 + 1], Base[Index * 3 + 2]) * 100.f;
		}
	};
	Read(0, OutBefore);
	Read(1, OutAfter);
	return true;
}

bool FMeshPositionMap::Apply(const TArray<FVector3f>& Snapshot, const TArray<FVector3f>& Before, const TArray<FVector3f>& After,
	TArray<FVector3f>& OutPositions, FString& OutError)
{
	if (Before.Num() == 0 || Before.Num() != After.Num())
	{
		OutError = TEXT("The positions sent back are missing or do not pair up.");
		return false;
	}

	// The arrivals are our own positions after a round trip in float - centimetres to metres and back - a few
	// hundred-thousandths of a centimetre off. Found by a grid, then by distance.
	constexpr float Cell = 0.05f;
	constexpr float Tolerance = 0.01f;
	auto Key = [](const FVector3f& P)
	{
		return FIntVector(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell), FMath::FloorToInt(P.Z / Cell));
	};

	TMultiMap<FIntVector, int32> Grid;
	Grid.Reserve(Before.Num());
	for (int32 Index = 0; Index < Before.Num(); ++Index)
	{
		Grid.Add(Key(Before[Index]), Index);
	}

	OutPositions.SetNumZeroed(Snapshot.Num());
	int32 Missing = 0;
	TArray<int32> Found;
	for (int32 Vertex = 0; Vertex < Snapshot.Num(); ++Vertex)
	{
		const FVector3f& Position = Snapshot[Vertex];
		const FIntVector Centre = Key(Position);
		FVector3f Sum = FVector3f::ZeroVector;
		int32 Count = 0;
		for (int32 X = -1; X <= 1; ++X)
		{
			for (int32 Y = -1; Y <= 1; ++Y)
			{
				for (int32 Z = -1; Z <= 1; ++Z)
				{
					Found.Reset();
					Grid.MultiFind(Centre + FIntVector(X, Y, Z), Found);
					for (const int32 Arrival : Found)
					{
						if (FVector3f::DistSquared(Before[Arrival], Position) <= Tolerance * Tolerance)
						{
							Sum += After[Arrival];
							++Count;
						}
					}
				}
			}
		}
		if (Count > 0)
		{
			OutPositions[Vertex] = Sum / float(Count);
		}
		else
		{
			// An id not in use sits at zero in the snapshot and needs nothing; anything else is a real miss.
			if (!Position.IsZero())
			{
				++Missing;
			}
			OutPositions[Vertex] = Position;
		}
	}

	if (Missing > 0)
	{
		OutError = FString::Printf(TEXT("%d of %d vertices were not found among the positions sent back, so the edit cannot be put on the mesh."),
			Missing, Snapshot.Num());
		return false;
	}
	return true;
}

#include "MeshForgeEditorSettings.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

UMeshForgeEditorSettings* UMeshForgeEditorSettings::Get()
{
	return GetMutableDefault<UMeshForgeEditorSettings>();
}

FString UMeshForgeEditorSettings::ResolveBlender() const
{
	// Copied from MeshForgeGarment's settings rather than shared: MeshForge must not depend on its own
	// add-on, and the family keeps a small duplicate over a link between plugins.
	if (!BlenderExecutable.FilePath.IsEmpty())
	{
		return FPaths::ConvertRelativePathToFull(BlenderExecutable.FilePath);
	}

#if PLATFORM_WINDOWS
	// Newest first. Two installs side by side is the common case on a machine that has had Blender for a
	// while, and the newer glTF add-on is the one worth round-tripping through.
	TArray<FString> Found;

	for (const TCHAR* Root : { TEXT("ProgramFiles"), TEXT("ProgramFiles(x86)") })
	{
		const FString Programs = FPlatformMisc::GetEnvironmentVariable(Root);

		if (Programs.IsEmpty())
		{
			continue;
		}

		TArray<FString> Versions;
		IFileManager::Get().FindFiles(Versions, *(Programs / TEXT("Blender Foundation") / TEXT("*")), false, true);

		for (const FString& Version : Versions)
		{
			const FString Exe = Programs / TEXT("Blender Foundation") / Version / TEXT("blender.exe");

			if (IFileManager::Get().FileExists(*Exe))
			{
				Found.Add(Exe);
			}
		}
	}

	if (Found.Num() > 0)
	{
		Found.Sort();
		return Found.Last();
	}

	return FString();
#else
	FString Output;
	int32 Code = -1;
	FPlatformProcess::ExecProcess(TEXT("/usr/bin/env"), TEXT("which blender"), &Code, &Output, nullptr);
	Output.TrimStartAndEndInline();
	return Code == 0 ? Output : FString();
#endif
}

#include "ForgeLibrary.h"

#include "MeshForge.h"

#include "Dom/JsonObject.h"
#include "Hash/Blake3.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace ForgeLibraryPrivate
{
	static const TCHAR* RecordFile = TEXT("take.json");
	static const TCHAR* InputsFolder = TEXT("inputs");

	/** A name that is safe as a folder on every platform, and still recognisable. */
	static FString Sanitise(const FString& In)
	{
		FString Out = In;

		for (const TCHAR Bad : { TEXT('/'), TEXT('\\'), TEXT(':'), TEXT('*'), TEXT('?'),
								 TEXT('"'), TEXT('<'), TEXT('>'), TEXT('|') })
		{
			Out.ReplaceCharInline(Bad, TEXT('_'));
		}

		return Out.IsEmpty() ? TEXT("Unnamed") : Out;
	}

	static FString ToIso(const FDateTime& When)
	{
		return (When == FDateTime::MinValue()) ? FString() : When.ToIso8601();
	}

	static FDateTime FromIso(const FString& Text)
	{
		FDateTime Parsed;
		return FDateTime::ParseIso8601(*Text, Parsed) ? Parsed : FDateTime::MinValue();
	}
}

bool FForgeTakeRecord::HasArtifact() const
{
	const FString Path = ArtifactPath();
	return !Path.IsEmpty() && FPaths::FileExists(Path);
}

FString FForgeTakeRecord::ArtifactPath() const
{
	return (Directory.IsEmpty() || OutputFile.IsEmpty())
		? FString()
		: FPaths::Combine(Directory, OutputFile);
}

FString FForgeLibrary::RootDirectory()
{
	// The project root, not Saved or Intermediate. Both of those are conventionally safe to delete,
	// which is the opposite of what a vault is for - see PROVENANCE_CONTRACT.md. Whether it is
	// committed, put in LFS or ignored is each project's decision and nothing here assumes one.
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectDir(), TEXT("Forge"), TEXT("Library")));
}

FString FForgeLibrary::TakeDirectory(
	const FString& Plugin, const FString& Definition, const FString& TakeId)
{
	return FPaths::Combine(
		RootDirectory(),
		ForgeLibraryPrivate::Sanitise(Plugin),
		ForgeLibraryPrivate::Sanitise(Definition),
		ForgeLibraryPrivate::Sanitise(TakeId));
}

FString FForgeLibrary::HashBytes(const TArray<uint8>& Bytes)
{
	if (Bytes.Num() == 0)
	{
		return FString();
	}

	const FBlake3Hash Hash = FBlake3::HashBuffer(Bytes.GetData(), Bytes.Num());
	return LexToString(Hash);
}

FString FForgeLibrary::HashFile(const FString& AbsolutePath)
{
	TArray<uint8> Bytes;

	// Read whole rather than streamed. Artifacts here are tens of megabytes, this runs once per
	// take, and a streaming hash would be more code for no measurable gain.
	return FFileHelper::LoadFileToArray(Bytes, *AbsolutePath) ? HashBytes(Bytes) : FString();
}

FString FForgeLibrary::CopyInput(const FString& TakeDirectory, const FString& SourceFile)
{
	if (TakeDirectory.IsEmpty() || !FPaths::FileExists(SourceFile))
	{
		return FString();
	}

	const FString Relative = FPaths::Combine(
		ForgeLibraryPrivate::InputsFolder, FPaths::GetCleanFilename(SourceFile));

	const FString Destination = FPaths::Combine(TakeDirectory, Relative);

	IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
	Files.CreateDirectoryTree(*FPaths::GetPath(Destination));

	// Never overwritten. A take folder is written once, and a second input with the same name is a
	// sign something is being reused rather than recorded.
	if (FPaths::FileExists(Destination) || Files.CopyFile(*Destination, *SourceFile))
	{
		return Relative;
	}

	UE_LOG(LogMeshForge, Warning, TEXT("Could not copy '%s' into the take's inputs."), *SourceFile);
	return FString();
}

bool FForgeLibrary::Deposit(FForgeTakeRecord& Record, const FString& SourceFile, FString& OutError)
{
	OutError.Reset();

	if (!Record.IsValid())
	{
		OutError = TEXT("A take with no id cannot be filed.");
		return false;
	}

	if (!FPaths::FileExists(SourceFile))
	{
		OutError = FString::Printf(TEXT("There is no file at '%s' to file."), *SourceFile);
		return false;
	}

	const FString Directory =
		TakeDirectory(Record.Plugin, Record.DefinitionName, Record.TakeId);

	IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();

	if (!Files.CreateDirectoryTree(*Directory))
	{
		OutError = FString::Printf(TEXT("Could not create '%s'."), *Directory);
		return false;
	}

	const FString Extension = FPaths::GetExtension(SourceFile, /*bIncludeDot*/ false);

	Record.Directory  = Directory;
	Record.OutputFile = Extension.IsEmpty()
		? TEXT("artifact")
		: FString::Printf(TEXT("artifact.%s"), *Extension);

	const FString Destination = FPaths::Combine(Directory, Record.OutputFile);

	// Not overwritten where it is already there. A re-download of the same take is the same bytes,
	// and rewriting it would change a file somebody may have referenced by hash.
	if (!FPaths::FileExists(Destination) && !Files.CopyFile(*Destination, *SourceFile))
	{
		OutError = FString::Printf(TEXT("Could not copy the artifact into '%s'."), *Directory);
		return false;
	}

	Record.OutputBytes = IFileManager::Get().FileSize(*Destination);
	Record.OutputHash  = HashFile(Destination);

	if (Record.EngineVersion.IsEmpty())
	{
		Record.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	}

	if (!WriteRecord(Record, OutError))
	{
		return false;
	}

	UE_LOG(LogMeshForge, Log, TEXT("Filed take %s (%s, %.1f MB) at %s"),
		*Record.TakeId, *Record.ProviderId, Record.OutputBytes / (1024.0 * 1024.0), *Directory);

	return true;
}

bool FForgeLibrary::WriteRecord(const FForgeTakeRecord& Record, FString& OutError)
{
	OutError.Reset();

	if (Record.Directory.IsEmpty())
	{
		OutError = TEXT("That take has no folder to write into.");
		return false;
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetNumberField(TEXT("schema"), Record.Schema);
	Root->SetStringField(TEXT("takeId"), Record.TakeId);
	Root->SetStringField(TEXT("plugin"), Record.Plugin);
	Root->SetStringField(TEXT("kind"), Record.Kind);
	Root->SetStringField(TEXT("status"), Record.Status);
	Root->SetStringField(TEXT("error"), Record.Error);

	const TSharedRef<FJsonObject> Definition = MakeShared<FJsonObject>();
	Definition->SetStringField(TEXT("path"), Record.DefinitionPath);
	Definition->SetStringField(TEXT("name"), Record.DefinitionName);
	Root->SetObjectField(TEXT("definition"), Definition);

	const TSharedRef<FJsonObject> Provider = MakeShared<FJsonObject>();
	Provider->SetStringField(TEXT("id"), Record.ProviderId);
	Provider->SetStringField(TEXT("model"), Record.ModelId);
	Provider->SetStringField(TEXT("endpoint"), Record.Endpoint);
	Provider->SetBoolField(TEXT("local"), Record.bLocal);
	Root->SetObjectField(TEXT("provider"), Provider);

	const TSharedRef<FJsonObject> Inputs = MakeShared<FJsonObject>();
	Inputs->SetStringField(TEXT("prompt"), Record.Prompt);

	TArray<TSharedPtr<FJsonValue>> Images;

	for (const FForgeTakeInput& Input : Record.Inputs)
	{
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("role"), Input.Role);

		if (!Input.AssetPath.IsEmpty()) { Entry->SetStringField(TEXT("asset"), Input.AssetPath); }
		if (!Input.File.IsEmpty())      { Entry->SetStringField(TEXT("file"), Input.File); }
		if (!Input.Hash.IsEmpty())      { Entry->SetStringField(TEXT("blake3"), Input.Hash); }

		Images.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Inputs->SetArrayField(TEXT("images"), Images);

	const TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();

	for (const TPair<FString, FString>& Pair : Record.Settings)
	{
		Settings->SetStringField(Pair.Key, Pair.Value);
	}

	Inputs->SetObjectField(TEXT("settings"), Settings);
	Root->SetObjectField(TEXT("inputs"), Inputs);

	const TSharedRef<FJsonObject> Cost = MakeShared<FJsonObject>();
	Cost->SetStringField(TEXT("currency"), Record.CostCurrency);
	Cost->SetNumberField(TEXT("amount"), Record.CostAmount);
	Cost->SetBoolField(TEXT("estimated"), Record.bCostEstimated);

	// In dollars as well as in the vendor's own unit, because credits are not comparable between
	// vendors and money is. Zero means the vendor publishes no rate, which the ledger says plainly
	// rather than guessing at.
	Cost->SetNumberField(TEXT("usd"), Record.CostUsd);
	Root->SetObjectField(TEXT("cost"), Cost);

	const TSharedRef<FJsonObject> Timing = MakeShared<FJsonObject>();
	Timing->SetStringField(TEXT("startedUtc"), ForgeLibraryPrivate::ToIso(Record.StartedUtc));
	Timing->SetStringField(TEXT("finishedUtc"), ForgeLibraryPrivate::ToIso(Record.FinishedUtc));
	Timing->SetNumberField(TEXT("seconds"), Record.Seconds);
	Root->SetObjectField(TEXT("timing"), Timing);

	const TSharedRef<FJsonObject> Output = MakeShared<FJsonObject>();
	Output->SetStringField(TEXT("file"), Record.OutputFile);
	Output->SetNumberField(TEXT("bytes"), static_cast<double>(Record.OutputBytes));
	Output->SetStringField(TEXT("blake3"), Record.OutputHash);
	Root->SetObjectField(TEXT("output"), Output);

	TArray<TSharedPtr<FJsonValue>> Processing;

	for (const FForgeProcessingEvent& Event : Record.Processing)
	{
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("stepId"), Event.StepId.ToString());
		Entry->SetStringField(TEXT("tool"), Event.Tool);
		Entry->SetStringField(TEXT("summary"), Event.Summary);
		Entry->SetStringField(TEXT("atUtc"), ForgeLibraryPrivate::ToIso(Event.AtUtc));

		Processing.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Root->SetArrayField(TEXT("processing"), Processing);

	Root->SetStringField(TEXT("engine"), Record.EngineVersion);
	Root->SetStringField(TEXT("toolVersion"), Record.ToolVersion);

	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);

	FJsonSerializer::Serialize(Root, Writer);

	// Pretty-printed on purpose. This is meant to be opened, read and diffed by a person as well as
	// scanned by a tool, and a minified ledger is one nobody ever looks at.
	const FString Path = FPaths::Combine(Record.Directory, ForgeLibraryPrivate::RecordFile);

	if (!FFileHelper::SaveStringToFile(Text, *Path))
	{
		OutError = FString::Printf(TEXT("Could not write '%s'."), *Path);
		return false;
	}

	return true;
}

bool FForgeLibrary::ReadRecord(const FString& InDirectory, FForgeTakeRecord& OutRecord)
{
	const FString Path = FPaths::Combine(InDirectory, ForgeLibraryPrivate::RecordFile);

	FString Text;

	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);

	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogMeshForge, Warning, TEXT("'%s' is not readable JSON."), *Path);
		return false;
	}

	OutRecord = FForgeTakeRecord();
	OutRecord.Directory = InDirectory;

	Root->TryGetNumberField(TEXT("schema"), OutRecord.Schema);
	Root->TryGetStringField(TEXT("takeId"), OutRecord.TakeId);
	Root->TryGetStringField(TEXT("plugin"), OutRecord.Plugin);
	Root->TryGetStringField(TEXT("kind"), OutRecord.Kind);
	Root->TryGetStringField(TEXT("status"), OutRecord.Status);
	Root->TryGetStringField(TEXT("error"), OutRecord.Error);

	const TSharedPtr<FJsonObject>* Object = nullptr;

	if (Root->TryGetObjectField(TEXT("definition"), Object) && Object)
	{
		(*Object)->TryGetStringField(TEXT("path"), OutRecord.DefinitionPath);
		(*Object)->TryGetStringField(TEXT("name"), OutRecord.DefinitionName);
	}

	if (Root->TryGetObjectField(TEXT("provider"), Object) && Object)
	{
		(*Object)->TryGetStringField(TEXT("id"), OutRecord.ProviderId);
		(*Object)->TryGetStringField(TEXT("model"), OutRecord.ModelId);
		(*Object)->TryGetStringField(TEXT("endpoint"), OutRecord.Endpoint);
		(*Object)->TryGetBoolField(TEXT("local"), OutRecord.bLocal);
	}

	if (Root->TryGetObjectField(TEXT("inputs"), Object) && Object)
	{
		(*Object)->TryGetStringField(TEXT("prompt"), OutRecord.Prompt);

		const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;

		if ((*Object)->TryGetArrayField(TEXT("images"), Images) && Images)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Images)
			{
				const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;

				if (!Entry.IsValid())
				{
					continue;
				}

				FForgeTakeInput Input;
				Entry->TryGetStringField(TEXT("role"), Input.Role);
				Entry->TryGetStringField(TEXT("asset"), Input.AssetPath);
				Entry->TryGetStringField(TEXT("file"), Input.File);
				Entry->TryGetStringField(TEXT("blake3"), Input.Hash);

				OutRecord.Inputs.Add(Input);
			}
		}

		const TSharedPtr<FJsonObject>* Settings = nullptr;

		if ((*Object)->TryGetObjectField(TEXT("settings"), Settings) && Settings)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Settings)->Values)
			{
				FString Value;

				if (Pair.Value.IsValid() && Pair.Value->TryGetString(Value))
				{
					OutRecord.Settings.Add(Pair.Key, Value);
				}
			}
		}
	}

	if (Root->TryGetObjectField(TEXT("cost"), Object) && Object)
	{
		(*Object)->TryGetStringField(TEXT("currency"), OutRecord.CostCurrency);
		(*Object)->TryGetNumberField(TEXT("amount"), OutRecord.CostAmount);
		(*Object)->TryGetBoolField(TEXT("estimated"), OutRecord.bCostEstimated);
		(*Object)->TryGetNumberField(TEXT("usd"), OutRecord.CostUsd);
	}

	if (Root->TryGetObjectField(TEXT("timing"), Object) && Object)
	{
		FString Started, Finished;
		(*Object)->TryGetStringField(TEXT("startedUtc"), Started);
		(*Object)->TryGetStringField(TEXT("finishedUtc"), Finished);
		(*Object)->TryGetNumberField(TEXT("seconds"), OutRecord.Seconds);

		OutRecord.StartedUtc  = ForgeLibraryPrivate::FromIso(Started);
		OutRecord.FinishedUtc = ForgeLibraryPrivate::FromIso(Finished);
	}

	if (Root->TryGetObjectField(TEXT("output"), Object) && Object)
	{
		double Bytes = 0.0;
		(*Object)->TryGetStringField(TEXT("file"), OutRecord.OutputFile);
		(*Object)->TryGetNumberField(TEXT("bytes"), Bytes);
		(*Object)->TryGetStringField(TEXT("blake3"), OutRecord.OutputHash);

		OutRecord.OutputBytes = static_cast<int64>(Bytes);
	}

	const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;

	if (Root->TryGetArrayField(TEXT("processing"), Events) && Events)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Events)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;

			if (!Entry.IsValid())
			{
				continue;
			}

			FForgeProcessingEvent Event;
			FString StepId, At;

			Entry->TryGetStringField(TEXT("stepId"), StepId);
			Entry->TryGetStringField(TEXT("tool"), Event.Tool);
			Entry->TryGetStringField(TEXT("summary"), Event.Summary);
			Entry->TryGetStringField(TEXT("atUtc"), At);

			Event.StepId = FName(*StepId);
			Event.AtUtc  = ForgeLibraryPrivate::FromIso(At);

			OutRecord.Processing.Add(Event);
		}
	}

	Root->TryGetStringField(TEXT("engine"), OutRecord.EngineVersion);
	Root->TryGetStringField(TEXT("toolVersion"), OutRecord.ToolVersion);

	return OutRecord.IsValid();
}

TArray<FForgeTakeRecord> FForgeLibrary::ListTakes(const FString& Plugin, const FString& Definition)
{
	TArray<FForgeTakeRecord> Takes;

	const FString Root = FPaths::Combine(
		RootDirectory(),
		ForgeLibraryPrivate::Sanitise(Plugin),
		ForgeLibraryPrivate::Sanitise(Definition));

	if (!FPaths::DirectoryExists(Root))
	{
		return Takes;
	}

	TArray<FString> Folders;
	IFileManager::Get().FindFiles(Folders, *(Root / TEXT("*")), /*Files*/ false, /*Directories*/ true);

	for (const FString& Folder : Folders)
	{
		FForgeTakeRecord Record;

		// A folder without a readable record is skipped rather than reported. Somebody may have put
		// something of their own in the vault, and it is their disk.
		if (ReadRecord(FPaths::Combine(Root, Folder), Record))
		{
			Takes.Add(MoveTemp(Record));
		}
	}

	// Newest first. What somebody wants from a history is nearly always the last few.
	Takes.Sort([](const FForgeTakeRecord& A, const FForgeTakeRecord& B)
	{
		return A.FinishedUtc > B.FinishedUtc;
	});

	return Takes;
}

bool FForgeLibrary::AppendProcessing(
	const FString& InDirectory, const FForgeProcessingEvent& Event, FString& OutError)
{
	FForgeTakeRecord Record;

	if (!ReadRecord(InDirectory, Record))
	{
		OutError = FString::Printf(TEXT("There is no take record at '%s'."), *InDirectory);
		return false;
	}

	Record.Processing.Add(Event);
	return WriteRecord(Record, OutError);
}

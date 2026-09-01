// The vault: every artifact ever generated, and the record of what made it.

#pragma once

#include "CoreMinimal.h"
#include "ForgeLibrary.generated.h"

/**
 * One thing that went into a generation.
 *
 * Either a project asset - recorded by path, because the project already holds it - or a file that
 * came from outside it, copied into the take's own `inputs/` folder because somebody's desktop is
 * not part of this project. Both carry a hash, which is what makes the trail hold when a file is
 * later moved, renamed or edited.
 */
USTRUCT(BlueprintType)
struct MESHFORGE_API FForgeTakeInput
{
	GENERATED_BODY()

	/** What it was for: "prompt", "main", "view1", "mesh". */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Input")
	FString Role;

	/** Content path, where this input is a project asset. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Input")
	FString AssetPath;

	/** Path inside the take folder, where it was copied in. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Input")
	FString File;

	/** Blake3 of the bytes. Empty where the input is text rather than a file. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Input")
	FString Hash;
};

/**
 * One thing that was done to a take after it was generated.
 *
 * Appended, never rewritten, because these are events: two fixes are two entries, and running the
 * same one twice is worth seeing. The same shape MotionForge already records on its clips.
 */
USTRUCT(BlueprintType)
struct MESHFORGE_API FForgeProcessingEvent
{
	GENERATED_BODY()

	/** A stable dotted id: `unreal.import`, `blender.normalise`, `meshy.retexture`. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Processing")
	FName StepId;

	/** What did it, with a version where it has one. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Processing")
	FString Tool;

	/** One line a person reads: "1,990,244 triangles, 3 LODs, Nanite on". */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Processing", meta = (MultiLine = true))
	FString Summary;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Processing")
	FDateTime AtUtc = FDateTime::MinValue();
};

/**
 * Everything known about one generated take. This is `take.json`.
 *
 * **Written the moment the artifact exists, before anything is imported.** An import can fail, an
 * editor can be closed, a definition can be deleted; none of that may lose the record of something
 * that was paid for. See PROVENANCE_CONTRACT.md for why this exists at all - the short version is a
 * Meshy take that was billed, succeeded and became unreachable because everything that knew how to
 * fetch it lived in memory.
 */
USTRUCT(BlueprintType)
struct MESHFORGE_API FForgeTakeRecord
{
	GENERATED_BODY()

	/** Schema version of this record. Bump when a field changes meaning, never when one is added. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	int32 Schema = 1;

	/**
	 * The provider's own job or task id, or a GUID where it has none.
	 *
	 * The primary key everywhere: it joins the vault, the ledger and the asset. Where a provider's
	 * ids are stable this is also what re-downloads the artifact years later.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FString TakeId;

	/** Which plugin generated it: MeshForge, MotionForge, SpeechForge, FaceForge. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FString Plugin;

	/** What kind of thing it is: mesh, image, motion, speech, face. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FString Kind;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Definition")
	FString DefinitionPath;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Definition")
	FString DefinitionName;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	FString ProviderId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	FString ModelId;

	/** The route it was submitted to, where the provider has more than one. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	FString Endpoint;

	/** True where it ran on this machine's hardware, so nothing was billed. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	bool bLocal = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Inputs", meta = (MultiLine = true))
	FString Prompt;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Inputs")
	TArray<FForgeTakeInput> Inputs;

	/** Everything the provider was told, flattened. What is here is what was actually sent. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Inputs")
	TMap<FString, FString> Settings;

	/** "meshy-credits", "usd", or empty where nothing was billed. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cost")
	FString CostCurrency;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cost")
	double CostAmount = 0.0;

	/**
	 * True while this is our estimate rather than the vendor's receipt.
	 *
	 * Not decoration. A ledger that cannot tell a quote from an invoice will be believed anyway, and
	 * most vendors bill on their own terms.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cost")
	bool bCostEstimated = true;

	/**
	 * What this take cost in US dollars, approximately. Zero means nobody could say.
	 *
	 * **Credits are not comparable across vendors and dollars are.** Thirty Meshy credits and thirty
	 * Tripo credits are different quantities of money, and a ledger that lists both in "credits" is a
	 * ledger nobody can add up. Tripo publishes an exact rate; Meshy publishes none, so its takes
	 * carry a zero here and the ledger says so rather than inventing a number.
	 *
	 * Approximate on purpose, and labelled that way wherever it is shown: a published rate is not an
	 * invoice, and plans, bundles and promotions all move it.
	 */
	double CostUsd = 0.0;

	/**
	 * How the take ended: "succeeded" or "failed". Empty on records written before this existed.
	 *
	 * **Failures are filed too, and that is the point of recording this.** A generation that failed
	 * after the provider accepted it has still been charged, and a library that only lists successes
	 * quietly understates what a definition cost - which is exactly the number somebody is trying to
	 * find when they open it.
	 */
	FString Status;

	/** Why it failed. Empty on a take that did not. */
	FString Error;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Timing")
	FDateTime StartedUtc = FDateTime::MinValue();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Timing")
	FDateTime FinishedUtc = FDateTime::MinValue();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Timing")
	float Seconds = 0.0f;

	/** File name inside the take folder, e.g. `artifact.glb`. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Output")
	FString OutputFile;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Output")
	int64 OutputBytes = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Output")
	FString OutputHash;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Output")
	TArray<FForgeProcessingEvent> Processing;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tool")
	FString EngineVersion;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tool")
	FString ToolVersion;

	/** Absolute path of the take folder. Filled on read; not written into the file. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FString Directory;

	bool IsValid() const { return !TakeId.IsEmpty(); }

	/** True when the artifact is still on this disk. False after somebody cleaned the vault. */
	bool HasArtifact() const;

	/** Absolute path to the artifact, whether or not it is still there. */
	FString ArtifactPath() const;
};

/**
 * The vault, and everything that reads or writes it.
 *
 * `<Project>/Forge/Library/<Plugin>/<Definition>/<TakeId>/` - see PROVENANCE_CONTRACT.md. Three
 * rules matter more than the layout:
 *
 * - **A take folder is written once.** Regenerating makes a new take id and a new folder; there is
 *   no "latest" file for anything to clobber. The bug that made this necessary was a single
 *   `MSD_PanelTest.glb` overwritten on every run, so a definition's own candidate list pointed at
 *   artifacts that no longer existed.
 * - **Nothing here deletes an artifact.** Cleaning the vault is a person's decision, made in a file
 *   browser. The record survives the cleaning, so a cleaned take can still say what it was and,
 *   where the provider's ids are stable, still be fetched again.
 * - **This is deliberately not aware of MeshForge.** It takes a plugin name as a string so the same
 *   code serves motion, speech and faces when they adopt it - which is the whole point of writing
 *   it as a contract first.
 */
class MESHFORGE_API FForgeLibrary
{
public:

	/** `<Project>/Forge/Library`, absolute. Created on demand. */
	static FString RootDirectory();

	/** The folder for one take, absolute. Does not create it. */
	static FString TakeDirectory(const FString& Plugin, const FString& Definition, const FString& TakeId);

	/**
	 * Put an artifact and its record into the vault.
	 *
	 * `SourceFile` is moved in by copy, keeping its extension. The record is completed with the
	 * output's size, hash and file name, and written beside it. Returns false with a sentence.
	 *
	 * Safe to call for a take that is already there: the artifact is not overwritten and the record
	 * is refreshed, which is what makes a re-download idempotent.
	 */
	static bool Deposit(FForgeTakeRecord& Record, const FString& SourceFile, FString& OutError);

	/** Write or rewrite a take's record without touching its artifact. */
	static bool WriteRecord(const FForgeTakeRecord& Record, FString& OutError);

	/** Read one take's record. False when there is no record there. */
	static bool ReadRecord(const FString& TakeDirectory, FForgeTakeRecord& OutRecord);

	/**
	 * Every take this definition has ever produced, newest first.
	 *
	 * Read from disk rather than from the definition, so a take survives the definition forgetting
	 * it - which is what happens when somebody deletes a candidate to tidy the list.
	 */
	static TArray<FForgeTakeRecord> ListTakes(const FString& Plugin, const FString& Definition);

	/** Add an event to a take's record. Appends; never rewrites what is there. */
	static bool AppendProcessing(
		const FString& TakeDirectory, const FForgeProcessingEvent& Event, FString& OutError);

	/**
	 * Blake3 of a file, as hex. Empty when it cannot be read.
	 *
	 * Blake3 rather than SHA-256 because it is what the engine ships - Core has no SHA-256 - and it
	 * is a better hash besides. The field is named for the algorithm so nobody has to guess.
	 */
	static FString HashFile(const FString& AbsolutePath);

	/** Blake3 of bytes already in memory, as hex. */
	static FString HashBytes(const TArray<uint8>& Bytes);

	/** Copy a file into a take's `inputs/`, returning the relative path recorded against it. */
	static FString CopyInput(const FString& TakeDirectory, const FString& SourceFile);
};

// VMVolumeManager.h
// VoluMatrix runtime loader for NRRD (intensity) volumes.
// Responsible ONLY for: reading .nhdr header, loading .raw, and creating a UVolumeTexture.
//
// We intentionally do NOT depend on internal Raymarcher structs here.
// Blueprints (or higher-level C++) will take the created UVolumeTexture and
// assign it to ARaymarchVolume.RaymarchResources / widgets, etc.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "VMVolumeManager.generated.h"

class UVolumeTexture;
class ARaymarchVolume;

// Simple struct describing only what we need from the NRRD header.
USTRUCT(BlueprintType)
struct FVMNRRDHeaderInfo
{
	GENERATED_BODY()

	// Dimensions in Unreal / VolumeTexture order.
	// We always store as X (columns), Y (rows), Z (slices).
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix|NRRD")
	int32 SizeX = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix|NRRD")
	int32 SizeY = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix|NRRD")
	int32 SizeZ = 0;

	// NRRD "type" field. We currently expect "short" (int16), matching the converter.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix|NRRD")
	FString Type;

	// Raw encoding mode, expected "raw".
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix|NRRD")
	FString Encoding;

	// Endianness, expected "little".
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix|NRRD")
	FString Endian;

	// Relative or absolute path to the .raw file as written in the NRRD header.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix|NRRD")
	FString DataFileName;

	// Optional: voxel spacing, purely informational here (Unreal uses unit cube).
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix|NRRD")
	FVector3f Spacing = FVector3f(1.0f, 1.0f, 1.0f);
};

UCLASS()
class TBRAYMARCHPROJECT_API AVMVolumeManager : public AActor
{
	GENERATED_BODY()

public:
	AVMVolumeManager();

	// Path to the .nhdr file on disk.
	// Example: D:/VM REPO/Volumatrix/Tools/ITKConverter/output/patient1.nhdr
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoluMatrix|NRRD")
	FString NrrdHeaderPath;

	// Optional: Raymarcher volume that this manager is "responsible" for.
	// You can leave this null and wire things manually in Blueprints if you prefer.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoluMatrix|Raymarcher")
	ARaymarchVolume* TargetRaymarchVolume;

	// The last successfully loaded volume texture.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix|NRRD")
	UVolumeTexture* LoadedIntensityTexture;

	// Parsed header for the last loaded NRRD (for debugging / tools).
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix|NRRD")
	FVMNRRDHeaderInfo LastHeaderInfo;

public:
	// High-level helper: load NRRD from NrrdHeaderPath and, if TargetRaymarchVolume is set,
	// assign the resulting UVolumeTexture to it via Blueprint or other logic.
	// This is primarily for convenience in the level.
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "VoluMatrix|NRRD")
	void LoadAndApplyNRRD();

	// Static low-level loader: given a .nhdr path, parse it, read the .raw file,
	// create a transient PF_G16 UVolumeTexture, and return it.
	// Returns nullptr on failure (check log for details).
	UFUNCTION(BlueprintCallable, Category = "VoluMatrix|NRRD")
	static UVolumeTexture* LoadNRRDIntensity(const FString& InNrrdHeaderPath, FVMNRRDHeaderInfo& OutHeaderInfo);

	// Optional helper that ONLY sets the provided intensity texture on a RaymarchVolume.
	// We don't touch VolumeAsset/ImageInfo here to avoid coupling to plugin internals.
	UFUNCTION(BlueprintCallable, Category = "VoluMatrix|Raymarcher")
	static void ApplyIntensityToRaymarcher(ARaymarchVolume* RaymarchVolume, UVolumeTexture* IntensityTexture);

protected:
	virtual void BeginPlay() override;

private:
	// ---- Internal implementation helpers (no UFUNCTION) ----

	// Parse the NRRD header at NhdrFilePath into OutHeader. Returns false on error.
	static bool ParseNRRDHeader(const FString& NhdrFilePath, FVMNRRDHeaderInfo& OutHeader, FString& OutError);

	// Read the RAW file referenced by Header.DataFileName into OutBytes.
	// NhdrFilePath is used as the base directory if DataFileName is relative.
	static bool LoadRawData(
		const FString& NhdrFilePath, const FVMNRRDHeaderInfo& Header, TArray<uint8>& OutBytes, FString& OutError);

	// Create a transient volume texture (PF_G16 / TSF_G16) from a 16-bit raw buffer.
	// Assumes bytes contain SizeX * SizeY * SizeZ int16 samples in the order written by the converter.
	static UVolumeTexture* CreateVolumeTextureFromRaw16(const FVMNRRDHeaderInfo& Header, const TArray<uint8>& Bytes);

	// Compute min / max intensity values from a 16-bit buffer. Purely diagnostic for now.
	static void ComputeVolumeMinMaxInt16(const TArray<uint8>& Bytes, int16& OutMin, int16& OutMax);
};

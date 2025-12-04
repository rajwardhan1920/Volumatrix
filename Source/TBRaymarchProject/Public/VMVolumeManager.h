// VMVolumeManager.h
// VoluMatrix runtime NRRD loader.
//
// This actor:
//   - Reads a simple 3D NRRD header (.nhdr)
//   - Loads the associated .raw file (16-bit signed intensity)
//   - Builds a transient UVolumeTexture (PF_G16)
//   - Exposes the resulting texture to Blueprint
//   - Optionally logs min/max so you can pick window/width

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "VMVolumeManager.generated.h"

class ARaymarchVolume;
class UVolumeTexture;

/**
 * Minimal parsed NRRD header info.
 * Assumptions (must match dicom_to_nrrd.py):
 *  - type: short
 *  - dimension: 3
 *  - encoding: raw
 *  - endian: little
 *  - sizes: Z Y X  (converter writes in that order)
 */
USTRUCT(BlueprintType)
struct FVMNRRDHeader
{
	GENERATED_BODY()

	// Texture dimensions in Unreal terms (VolumeTexture expects X,Y,Z)
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix")
	int32 SizeX = 0;	// columns (Nrrd axis 2)
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix")
	int32 SizeY = 0;	// rows    (Nrrd axis 1)
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix")
	int32 SizeZ = 0;	// slices  (Nrrd axis 0)

	// Full absolute path to the raw file
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix")
	FString RawFilePath;

	// Bytes per voxel (we use 2 for int16)
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix")
	int32 BytesPerVoxel = 2;

	// Endianness
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix")
	bool bLittleEndian = true;

	// Intensity range computed from RAW
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix")
	int32 MinValue = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix")
	int32 MaxValue = 0;
};

UCLASS(Blueprintable)
class TBRAYMARCHPROJECT_API AVMVolumeManager : public AActor
{
	GENERATED_BODY()

public:
	AVMVolumeManager();

	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Optional: RaymarchVolume we want to drive. We don't touch its internals in C++ anymore. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoluMatrix")
	ARaymarchVolume* TargetRaymarchVolume;

	/**
	 * Absolute path to the .nhdr file.
	 * Example:
	 *   D:/VM REPO/Volumatrix/Tools/ITKConverter/output/patient1.nhdr
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoluMatrix", meta = (FilePathFilter = "nhdr"))
	FString NRRDPath;

	/** True if last LoadNRRDIntensity() run fully succeeded. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix")
	bool bVolumeLoadedSuccessfully;

	/** Parsed header of the last loaded NRRD. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix")
	FVMNRRDHeader LastHeader;

	/**
	 * Main entry point for Blueprint / Level.
	 * Uses NRRDPath, parses header, loads RAW, creates UVolumeTexture.
	 */
	UFUNCTION(BlueprintCallable, Category = "VoluMatrix")
	void LoadNRRDIntensity();

	/** Get the currently loaded volume texture (may be null). */
	UFUNCTION(BlueprintPure, Category = "VoluMatrix")
	UVolumeTexture* GetLoadedVolumeTexture() const
	{
		return LoadedVolumeTexture;
	}

private:
	/** Keep a reference so GC does not delete the transient volume texture. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoluMatrix", meta = (AllowPrivateAccess = "true"))
	UVolumeTexture* LoadedVolumeTexture;

	// --- Internal helpers ---

	/** Parse minimal 3D NRRD header from disk into FVMNRRDHeader. */
	bool ParseNRRDHeader(const FString& HeaderFilePath, FVMNRRDHeader& OutHeader) const;

	/** Load RAW file, compute min/max, write back to OutHeader. */
	bool LoadRawDataAndComputeMinMax(FVMNRRDHeader& InOutHeader, TArray<uint8>& OutRawBytes) const;

	/** Create a PF_G16 UVolumeTexture from raw bytes. */
	UVolumeTexture* CreateVolumeTextureFromRaw(const FVMNRRDHeader& Header, const TArray<uint8>& RawBytes);

	/** Just log and sanity-check; no direct RaymarchResources access here. */
	void ApplyToRaymarchVolume(UVolumeTexture* VolumeTexture, const FVMNRRDHeader& Header);
};

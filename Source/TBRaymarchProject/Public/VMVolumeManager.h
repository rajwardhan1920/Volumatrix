#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

// Raymarcher plugin
#include "Actor/RaymarchVolume.h"

#include "VMVolumeManager.generated.h"

class UVolumeTexture;

USTRUCT()
struct FVMNRRDHeaderInfo
{
	GENERATED_BODY()

	int32 SizeX = 0;
	int32 SizeY = 0;
	int32 SizeZ = 0;

	FString DataFileName;
	FString Type;
	FString Encoding;
	FString Endian;
};

UCLASS()
class TBRAYMARCHPROJECT_API AVMVolumeManager : public AActor
{
	GENERATED_BODY()

public:
	AVMVolumeManager();

protected:
	virtual void BeginPlay() override;

public:
	/** Load NRRD (.nhdr + .raw) into a transient UVolumeTexture */
	UFUNCTION(BlueprintCallable, Category = "VoluMatrix|NRRD")
	static UVolumeTexture* LoadNRRDIntensity(const FString& NrrdHeaderPath);

	/** Apply intensity volume to ARaymarchVolume */
	UFUNCTION(BlueprintCallable, Category = "VoluMatrix|Raymarcher")
	static void ApplyIntensityToRaymarcher(ARaymarchVolume* TargetVolume, UVolumeTexture* IntensityTex);

private:
	static bool ParseNRRDHeader(const FString& NhdrFilePath, FVMNRRDHeaderInfo& OutHeader, FString& OutError);

	static bool LoadRawData(
		const FString& NhdrFilePath, const FVMNRRDHeaderInfo& Header, TArray<uint8>& OutBytes, FString& OutError);

	static UVolumeTexture* CreateVolumeTextureFromRaw16(const FVMNRRDHeaderInfo& Header, const TArray<uint8>& Bytes);
};

#include "VMVolumeManager.h"

#include "Engine/VolumeTexture.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogVMVolumeManager, Log, All);

AVMVolumeManager::AVMVolumeManager()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AVMVolumeManager::BeginPlay()
{
	Super::BeginPlay();
}

// ---------------------------------------------------------
// Helper: parse "sizes: 195 512 512"
// ---------------------------------------------------------
static bool ParseIntList(const FString& InString, TArray<int32>& OutValues)
{
	OutValues.Empty();
	TArray<FString> Parts;
	InString.ParseIntoArrayWS(Parts);
	for (const FString& Part : Parts)
	{
		int32 V = 0;
		if (LexTryParseString(V, *Part))
		{
			OutValues.Add(V);
		}
	}
	return OutValues.Num() > 0;
}

// ---------------------------------------------------------
// Parse NRRD header
// ---------------------------------------------------------
bool AVMVolumeManager::ParseNRRDHeader(const FString& NhdrFilePath, FVMNRRDHeaderInfo& OutHeader, FString& OutError)
{
	OutHeader = FVMNRRDHeaderInfo{};
	FString FileText;

	if (!FFileHelper::LoadFileToString(FileText, *NhdrFilePath))
	{
		OutError = TEXT("Failed to read header.");
		return false;
	}

	TArray<FString> Lines;
	FileText.ParseIntoArrayLines(Lines);

	if (Lines.Num() == 0 || !Lines[0].StartsWith(TEXT("NRRD")))
	{
		OutError = TEXT("Not a valid NRRD header");
		return false;
	}

	for (const FString& Line : Lines)
	{
		if (Line.StartsWith("#") || !Line.Contains(":"))
		{
			continue;
		}

		FString Key, Value;
		Line.Split(TEXT(":"), &Key, &Value);
		Key = Key.TrimStartAndEnd();
		Value = Value.TrimStartAndEnd();

		if (Key.Equals(TEXT("sizes")))
		{
			TArray<int32> S;
			if (!ParseIntList(Value, S) || S.Num() != 3)
			{
				OutError = TEXT("Invalid sizes.");
				return false;
			}

			// Our exporter uses Z Y X order
			OutHeader.SizeZ = S[0];
			OutHeader.SizeY = S[1];
			OutHeader.SizeX = S[2];
		}
		else if (Key.Equals(TEXT("type")))
		{
			OutHeader.Type = Value;
		}
		else if (Key.Equals(TEXT("encoding")))
		{
			OutHeader.Encoding = Value;
		}
		else if (Key.Equals(TEXT("endian")))
		{
			OutHeader.Endian = Value;
		}
		else if (Key.Equals(TEXT("data file")))
		{
			OutHeader.DataFileName = Value;
		}
	}

	if (OutHeader.DataFileName.IsEmpty())
	{
		OutError = TEXT("Missing data file.");
		return false;
	}

	return true;
}

// ---------------------------------------------------------
// Load RAW
// ---------------------------------------------------------
bool AVMVolumeManager::LoadRawData(
	const FString& NhdrFilePath, const FVMNRRDHeaderInfo& Header, TArray<uint8>& OutBytes, FString& OutError)
{
	OutBytes.Empty();

	FString Dir = FPaths::GetPath(NhdrFilePath);
	FString RawPath = Dir / Header.DataFileName;

	if (!FPaths::FileExists(RawPath))
	{
		OutError = FString::Printf(TEXT("RAW file missing: %s"), *RawPath);
		return false;
	}

	if (!FFileHelper::LoadFileToArray(OutBytes, *RawPath))
	{
		OutError = TEXT("Failed to read RAW");
		return false;
	}

	return true;
}

// ---------------------------------------------------------
// Create UVolumeTexture (UE 5.4 API)
// ---------------------------------------------------------
UVolumeTexture* AVMVolumeManager::CreateVolumeTextureFromRaw16(const FVMNRRDHeaderInfo& Header, const TArray<uint8>& Bytes)
{
	const int32 SX = Header.SizeX;
	const int32 SY = Header.SizeY;
	const int32 SZ = Header.SizeZ;

	UVolumeTexture* VolTex = NewObject<UVolumeTexture>(GetTransientPackage());
	if (!VolTex)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Failed to allocate VolumeTexture."));
		return nullptr;
	}

	VolTex->bNotOfflineProcessed = true;
	VolTex->CompressionSettings = TC_Default;
	VolTex->MipGenSettings = TMGS_NoMipmaps;
	VolTex->SRGB = false;

	// UE 5.4 Source.Init()
	VolTex->Source.Init(SX, SY, SZ, 1, TSF_G16);

	uint8* Dest = VolTex->Source.LockMip(0);
	const int64 ExpectedBytes = int64(SX) * SY * SZ * 2;

	const int64 CopyBytes = FMath::Min<int64>(ExpectedBytes, Bytes.Num());
	FMemory::Memcpy(Dest, Bytes.GetData(), CopyBytes);

	if (CopyBytes < ExpectedBytes)
	{
		FMemory::Memset(Dest + CopyBytes, 0, ExpectedBytes - CopyBytes);
	}

	VolTex->Source.UnlockMip(0);
	VolTex->UpdateResource();

	UE_LOG(LogVMVolumeManager, Log, TEXT("Created VolumeTexture %dx%dx%d"), SX, SY, SZ);

	return VolTex;
}

// ---------------------------------------------------------
// Public: Load a NRRD intensity volume
// ---------------------------------------------------------
UVolumeTexture* AVMVolumeManager::LoadNRRDIntensity(const FString& NrrdHeaderPath)
{
	FString CleanPath = NrrdHeaderPath;
	FPaths::NormalizeFilename(CleanPath);

	FVMNRRDHeaderInfo Header;
	FString Error;

	if (!ParseNRRDHeader(CleanPath, Header, Error))
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Header parse failed: %s"), *Error);
		return nullptr;
	}

	TArray<uint8> Bytes;
	if (!LoadRawData(CleanPath, Header, Bytes, Error))
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Raw load failed: %s"), *Error);
		return nullptr;
	}

	return CreateVolumeTextureFromRaw16(Header, Bytes);
}

// ---------------------------------------------------------
// Apply texture to raymarcher
// ---------------------------------------------------------
void AVMVolumeManager::ApplyIntensityToRaymarcher(ARaymarchVolume* TargetVolume, UVolumeTexture* IntensityTex)
{
	if (!TargetVolume)
	{
		UE_LOG(LogVMVolumeManager, Warning, TEXT("TargetVolume null."));
		return;
	}

	if (!IntensityTex)
	{
		UE_LOG(LogVMVolumeManager, Warning, TEXT("IntensityTex null."));
		return;
	}

	// 1) Plug the texture (THIS IS THE CORRECT FIELD)
	TargetVolume->RaymarchResources.DataVolumeTextureRef = IntensityTex;

	// 2) Switch to intensity-raymarch mode
	TargetVolume->SelectRaymarchMaterial = ERaymarchMaterial::Intensity;

	// 3) Push parameters
	TargetVolume->SetAllMaterialParameters();

	UE_LOG(LogVMVolumeManager, Log, TEXT("Applied NRRD intensity texture to RaymarchVolume."));
}

// VoluMatrix volume manager implementation.

#include "VMVolumeManager.h"

#include "Engine/VolumeTexture.h"
#include "HAL/PlatformFilemanager.h"
#include "Logging/LogMacros.h"
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

// ------------------------
// NRRD parsing helpers
// ------------------------

static bool ParseIntList(const FString& InString, TArray<int32>& OutValues)
{
	OutValues.Empty();
	TArray<FString> Parts;
	InString.ParseIntoArrayWS(Parts);

	for (const FString& Part : Parts)
	{
		if (Part.IsEmpty())
		{
			continue;
		}

		int32 Value = 0;
		if (!LexTryParseString(Value, *Part))
		{
			return false;
		}
		OutValues.Add(Value);
	}

	return OutValues.Num() > 0;
}

bool AVMVolumeManager::ParseNRRDHeader(const FString& NhdrFilePath, FVMNRRDHeaderInfo& OutHeader, FString& OutError)
{
	OutHeader = FVMNRRDHeaderInfo{};
	OutError.Empty();

	FString FileText;
	if (!FFileHelper::LoadFileToString(FileText, *NhdrFilePath))
	{
		OutError = FString::Printf(TEXT("Failed to read NRRD header: %s"), *NhdrFilePath);
		return false;
	}

	TArray<FString> Lines;
	FileText.ParseIntoArrayLines(Lines);

	if (Lines.Num() == 0 || !Lines[0].StartsWith(TEXT("NRRD")))
	{
		OutError = TEXT("File does not look like a NRRD header (missing NRRD000X line).");
		return false;
	}

	for (int32 i = 1; i < Lines.Num(); ++i)
	{
		const FString& Line = Lines[i];

		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
		{
			continue;
		}

		FString Key, Value;
		if (!Line.Split(TEXT(":"), &Key, &Value))
		{
			continue;
		}

		Key = Key.TrimStartAndEnd();
		Value = Value.TrimStartAndEnd();

		if (Key.Equals(TEXT("sizes"), ESearchCase::IgnoreCase))
		{
			TArray<int32> Sizes;
			if (!ParseIntList(Value, Sizes) || Sizes.Num() != 3)
			{
				OutError = TEXT("Invalid sizes line in NRRD header.");
				return false;
			}

			// Our exporter writes sizes: Z Y X
			OutHeader.SizeZ = Sizes[0];
			OutHeader.SizeY = Sizes[1];
			OutHeader.SizeX = Sizes[2];
		}
		else if (Key.Equals(TEXT("type"), ESearchCase::IgnoreCase))
		{
			OutHeader.Type = Value;
		}
		else if (Key.Equals(TEXT("encoding"), ESearchCase::IgnoreCase))
		{
			OutHeader.Encoding = Value;
		}
		else if (Key.Equals(TEXT("endian"), ESearchCase::IgnoreCase))
		{
			OutHeader.Endian = Value;
		}
		else if (Key.Equals(TEXT("data file"), ESearchCase::IgnoreCase))
		{
			OutHeader.DataFileName = Value;
		}
	}

	if (OutHeader.SizeX <= 0 || OutHeader.SizeY <= 0 || OutHeader.SizeZ <= 0)
	{
		OutError = TEXT("NRRD header missing valid sizes.");
		return false;
	}

	if (OutHeader.DataFileName.IsEmpty())
	{
		OutError = TEXT("NRRD header missing data file field.");
		return false;
	}

	if (OutHeader.Type.IsEmpty())
	{
		OutError = TEXT("NRRD header missing type field.");
		return false;
	}

	if (OutHeader.Encoding.IsEmpty())
	{
		OutError = TEXT("NRRD header missing encoding field.");
		return false;
	}

	return true;
}

bool AVMVolumeManager::LoadRawData(
	const FString& NhdrFilePath, const FVMNRRDHeaderInfo& Header, TArray<uint8>& OutBytes, FString& OutError)
{
	OutBytes.Empty();
	OutError.Empty();

	if (!Header.Encoding.Equals(TEXT("raw"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(TEXT("Only encoding=raw supported, got: %s"), *Header.Encoding);
		return false;
	}

	if (!Header.Type.Equals(TEXT("short"), ESearchCase::IgnoreCase) && !Header.Type.Equals(TEXT("int16"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(TEXT("Only type=short/int16 supported, got: %s"), *Header.Type);
		return false;
	}

	const FString NhdrDir = FPaths::GetPath(NhdrFilePath);
	const FString RawPath = FPaths::ConvertRelativePathToFull(NhdrDir / Header.DataFileName);

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.FileExists(*RawPath))
	{
		OutError = FString::Printf(TEXT("Raw data file not found: %s"), *RawPath);
		return false;
	}

	if (!FFileHelper::LoadFileToArray(OutBytes, *RawPath))
	{
		OutError = FString::Printf(TEXT("Failed to read raw data file: %s"), *RawPath);
		return false;
	}

	const int64 ExpectedBytes = int64(Header.SizeX) * Header.SizeY * Header.SizeZ * 2;
	if (OutBytes.Num() != ExpectedBytes)
	{
		UE_LOG(LogVMVolumeManager, Warning, TEXT("Raw size (%d) != expected (%lld) for %dx%dx%d"), OutBytes.Num(), ExpectedBytes,
			Header.SizeX, Header.SizeY, Header.SizeZ);
	}

	return true;
}

UVolumeTexture* AVMVolumeManager::CreateVolumeTextureFromRaw16(const FVMNRRDHeaderInfo& Header, const TArray<uint8>& Bytes)
{
	if (Header.SizeX <= 0 || Header.SizeY <= 0 || Header.SizeZ <= 0)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Invalid volume dimensions."));
		return nullptr;
	}

	const int32 SizeX = Header.SizeX;
	const int32 SizeY = Header.SizeY;
	const int32 SizeZ = Header.SizeZ;

	UVolumeTexture* VolumeTex = NewObject<UVolumeTexture>(GetTransientPackage());
	if (!VolumeTex)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Failed to create UVolumeTexture."));
		return nullptr;
	}

	VolumeTex->bNotOfflineProcessed = true;
	VolumeTex->CompressionSettings = TC_Default;
	VolumeTex->MipGenSettings = TMGS_NoMipmaps;
	VolumeTex->SRGB = false;

	// UE 5.4: width, height, NumSlices, NumMips, format
	VolumeTex->Source.Init(SizeX, SizeY, SizeZ, 1, ETextureSourceFormat::TSF_G16);

	uint8* DestData = VolumeTex->Source.LockMip(0);
	const int64 DestBytes = int64(SizeX) * SizeY * SizeZ * 2;

	const int64 CopyBytes = FMath::Min<int64>(DestBytes, (int64) Bytes.Num());
	if (CopyBytes > 0)
	{
		FMemory::Memcpy(DestData, Bytes.GetData(), CopyBytes);
	}
	if (CopyBytes < DestBytes)
	{
		FMemory::Memset(DestData + CopyBytes, 0, DestBytes - CopyBytes);
	}

	VolumeTex->Source.UnlockMip(0);
	VolumeTex->UpdateResource();

	UE_LOG(LogVMVolumeManager, Log, TEXT("Created VolumeTexture (%dx%dx%d) from RAW, bytes=%d."), SizeX, SizeY, SizeZ, Bytes.Num());

	return VolumeTex;
}

// ------------------------
// Public Blueprint API
// ------------------------

UVolumeTexture* AVMVolumeManager::LoadNRRDIntensity(const FString& NrrdHeaderPath)
{
	FString CleanPath = NrrdHeaderPath;
	FPaths::NormalizeFilename(CleanPath);

	FVMNRRDHeaderInfo Header;
	FString Error;

	if (!ParseNRRDHeader(CleanPath, Header, Error))
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("ParseNRRDHeader failed: %s"), *Error);
		return nullptr;
	}

	TArray<uint8> RawBytes;
	if (!LoadRawData(CleanPath, Header, RawBytes, Error))
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("LoadRawData failed: %s"), *Error);
		return nullptr;
	}

	UVolumeTexture* VolumeTexture = CreateVolumeTextureFromRaw16(Header, RawBytes);
	if (!VolumeTexture)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("CreateVolumeTextureFromRaw16 failed."));
		return nullptr;
	}

	UE_LOG(LogVMVolumeManager, Log, TEXT("Loaded NRRD '%s' as VolumeTexture (%dx%dx%d)."), *CleanPath, Header.SizeX, Header.SizeY,
		Header.SizeZ);

	return VolumeTexture;
}

void AVMVolumeManager::ApplyIntensityToRaymarcher(ARaymarchVolume* TargetVolume, UVolumeTexture* IntensityTex)
{
	if (!TargetVolume)
	{
		UE_LOG(LogVMVolumeManager, Warning, TEXT("ApplyIntensityToRaymarcher: TargetVolume is null."));
		return;
	}

	if (!IntensityTex)
	{
		UE_LOG(LogVMVolumeManager, Warning, TEXT("ApplyIntensityToRaymarcher: IntensityTex is null."));
		return;
	}

	// Plug our texture into the raymarch resources
	TargetVolume->RaymarchResources.DataVolume = IntensityTex;

	// Use the intensity material (simpler path, avoids light volume for now)
	TargetVolume->SelectRaymarchMaterial = ERaymarchMaterial::Intensity;

	// Push parameters so the materials see the new volume
	TargetVolume->SetAllMaterialParameters();

	UE_LOG(LogVMVolumeManager, Log, TEXT("ApplyIntensityToRaymarcher: applied volume to raymarcher."));
}

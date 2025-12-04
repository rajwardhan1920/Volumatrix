// VMVolumeManager.cpp
// VoluMatrix: NRRD (.nhdr + .raw) -> UVolumeTexture loader.
// This version avoids engine-internal FTexturePlatformData / FTexture3DMipMap
// and does NOT touch Raymarcher plugin internals. Blueprint will wire it.

#include "VMVolumeManager.h"

#include "Actor/RaymarchVolume.h"
#include "Engine/VolumeTexture.h"
#include "HAL/FileManager.h"
#include "Logging/LogMacros.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogVMVolumeManager, Log, All);

AVMVolumeManager::AVMVolumeManager()
{
	PrimaryActorTick.bCanEverTick = false;
	bVolumeLoadedSuccessfully = false;
	LoadedVolumeTexture = nullptr;
}

void AVMVolumeManager::BeginPlay()
{
	Super::BeginPlay();

	if (!NRRDPath.IsEmpty())
	{
		LoadNRRDIntensity();
	}
}

#if WITH_EDITOR
void AVMVolumeManager::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName ChangedPropName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;

	// Optional: auto-load when changing path in editor
	if (ChangedPropName == GET_MEMBER_NAME_CHECKED(AVMVolumeManager, NRRDPath))
	{
		// LoadNRRDIntensity();
	}
}
#endif

void AVMVolumeManager::LoadNRRDIntensity()
{
	bVolumeLoadedSuccessfully = false;
	LoadedVolumeTexture = nullptr;
	LastHeader = FVMNRRDHeader();

	if (NRRDPath.IsEmpty())
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("NRRDPath is empty on %s"), *GetName());
		return;
	}

	const FString AbsHeaderPath = FPaths::ConvertRelativePathToFull(NRRDPath);
	if (!FPaths::FileExists(AbsHeaderPath))
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("NRRD header not found: %s"), *AbsHeaderPath);
		return;
	}

	FVMNRRDHeader Header;
	if (!ParseNRRDHeader(AbsHeaderPath, Header))
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Failed to parse NRRD header: %s"), *AbsHeaderPath);
		return;
	}

	TArray<uint8> RawBytes;
	if (!LoadRawDataAndComputeMinMax(Header, RawBytes))
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Failed to load RAW data for NRRD: %s"), *AbsHeaderPath);
		return;
	}

	UVolumeTexture* VolumeTex = CreateVolumeTextureFromRaw(Header, RawBytes);
	if (!VolumeTex)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Failed to create UVolumeTexture from NRRD: %s"), *AbsHeaderPath);
		return;
	}

	LoadedVolumeTexture = VolumeTex;
	LastHeader = Header;
	bVolumeLoadedSuccessfully = true;

	ApplyToRaymarchVolume(VolumeTex, Header);

	UE_LOG(LogVMVolumeManager, Log, TEXT("Loaded NRRD '%s' -> %dx%dx%d, min=%d, max=%d"), *AbsHeaderPath, Header.SizeX,
		Header.SizeY, Header.SizeZ, Header.MinValue, Header.MaxValue);
}

// -------------------------------------------------------------------------
// NRRD parsing
// -------------------------------------------------------------------------

bool AVMVolumeManager::ParseNRRDHeader(const FString& HeaderFilePath, FVMNRRDHeader& OutHeader) const
{
	FString FileText;
	if (!FFileHelper::LoadFileToString(FileText, *HeaderFilePath))
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Failed to read NRRD header file: %s"), *HeaderFilePath);
		return false;
	}

	TArray<FString> Lines;
	FileText.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

	if (Lines.Num() == 0 || !Lines[0].StartsWith(TEXT("NRRD")))
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("File '%s' is not a valid NRRD (missing NRRD000x magic)."), *HeaderFilePath);
		return false;
	}

	int32 Sizes[3] = {0, 0, 0};
	FString DataFileRel;
	bool bLittleEndian = true;

	for (const FString& RawLine : Lines)
	{
		const FString Line = RawLine.TrimStartAndEnd();

		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
		{
			continue;
		}

		if (Line.StartsWith(TEXT("type:")))
		{
			const FString Value = Line.Mid(5).TrimStartAndEnd();
			if (!Value.Equals(TEXT("short"), ESearchCase::IgnoreCase))
			{
				UE_LOG(LogVMVolumeManager, Warning, TEXT("NRRD type '%s' in '%s' is not 'short'. Loader assumes int16."), *Value,
					*HeaderFilePath);
			}
		}
		else if (Line.StartsWith(TEXT("dimension:")))
		{
			const FString Value = Line.Mid(10).TrimStartAndEnd();
			const int32 Dim = FCString::Atoi(*Value);
			if (Dim != 3)
			{
				UE_LOG(LogVMVolumeManager, Error, TEXT("NRRD dimension %d in '%s' is not 3. Only 3D volumes are supported."), Dim,
					*HeaderFilePath);
				return false;
			}
		}
		else if (Line.StartsWith(TEXT("sizes:")))
		{
			// Example: sizes: 195 512 512 (Z Y X)
			const FString Value = Line.Mid(6).TrimStartAndEnd();
			TArray<FString> Tokens;
			Value.ParseIntoArray(Tokens, TEXT(" "), true);

			if (Tokens.Num() != 3)
			{
				UE_LOG(LogVMVolumeManager, Error, TEXT("NRRD 'sizes' must have 3 entries in '%s'. Found: %d"), *HeaderFilePath,
					Tokens.Num());
				return false;
			}

			Sizes[0] = FCString::Atoi(*Tokens[0]);	  // Z
			Sizes[1] = FCString::Atoi(*Tokens[1]);	  // Y
			Sizes[2] = FCString::Atoi(*Tokens[2]);	  // X
		}
		else if (Line.StartsWith(TEXT("endian:")))
		{
			const FString Value = Line.Mid(7).TrimStartAndEnd();
			bLittleEndian = !Value.Equals(TEXT("big"), ESearchCase::IgnoreCase);
		}
		else if (Line.StartsWith(TEXT("data file:")))
		{
			// Example: data file: patient1.raw
			const FString Value = Line.Mid(10).TrimStartAndEnd();
			DataFileRel = Value;
		}
	}

	if (Sizes[0] <= 0 || Sizes[1] <= 0 || Sizes[2] <= 0)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("NRRD '%s' has invalid sizes: %d %d %d"), *HeaderFilePath, Sizes[0], Sizes[1],
			Sizes[2]);
		return false;
	}

	if (DataFileRel.IsEmpty())
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("NRRD '%s' missing 'data file' entry."), *HeaderFilePath);
		return false;
	}

	OutHeader.SizeZ = Sizes[0];
	OutHeader.SizeY = Sizes[1];
	OutHeader.SizeX = Sizes[2];
	OutHeader.BytesPerVoxel = 2;
	OutHeader.bLittleEndian = bLittleEndian;

	const FString HeaderDir = FPaths::GetPath(HeaderFilePath);
	OutHeader.RawFilePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(HeaderDir, DataFileRel));

	return true;
}

// -------------------------------------------------------------------------
// RAW loading + min/max
// -------------------------------------------------------------------------

bool AVMVolumeManager::LoadRawDataAndComputeMinMax(FVMNRRDHeader& InOutHeader, TArray<uint8>& OutRawBytes) const
{
	if (!FPaths::FileExists(InOutHeader.RawFilePath))
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("NRRD RAW file not found: %s"), *InOutHeader.RawFilePath);
		return false;
	}

	const int64 ExpectedBytes = static_cast<int64>(InOutHeader.SizeX) * static_cast<int64>(InOutHeader.SizeY) *
								static_cast<int64>(InOutHeader.SizeZ) * static_cast<int64>(InOutHeader.BytesPerVoxel);

	int64 ActualSize = IFileManager::Get().FileSize(*InOutHeader.RawFilePath);
	if (ActualSize != ExpectedBytes)
	{
		UE_LOG(LogVMVolumeManager, Warning, TEXT("RAW file size mismatch for '%s': expected %lld bytes, got %lld bytes."),
			*InOutHeader.RawFilePath, ExpectedBytes, ActualSize);
	}

	if (!FFileHelper::LoadFileToArray(OutRawBytes, *InOutHeader.RawFilePath))
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Failed to read RAW file: %s"), *InOutHeader.RawFilePath);
		return false;
	}

	// Clamp or pad to expected bytes
	if (OutRawBytes.Num() < ExpectedBytes)
	{
		UE_LOG(LogVMVolumeManager, Warning, TEXT("RAW file '%s' is smaller than expected (%d < %lld). Zero-padding."),
			*InOutHeader.RawFilePath, OutRawBytes.Num(), ExpectedBytes);
		OutRawBytes.SetNumZeroed(ExpectedBytes);
	}
	else if (OutRawBytes.Num() > ExpectedBytes)
	{
		OutRawBytes.SetNum(ExpectedBytes);
	}

	// Interpret as int16
	if (OutRawBytes.Num() <= 0)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("RAW data empty after load."));
		return false;
	}

	const int16* DataPtr = reinterpret_cast<const int16*>(OutRawBytes.GetData());
	const int32 NumSamples = OutRawBytes.Num() / 2;
	if (NumSamples <= 0)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("RAW data has no 16-bit samples."));
		return false;
	}

	int16 MinVal = DataPtr[0];
	int16 MaxVal = DataPtr[0];

	for (int32 i = 1; i < NumSamples; ++i)
	{
		const int16 V = DataPtr[i];
		if (V < MinVal)
			MinVal = V;
		if (V > MaxVal)
			MaxVal = V;
	}

	InOutHeader.MinValue = MinVal;
	InOutHeader.MaxValue = MaxVal;

	return true;
}

// -------------------------------------------------------------------------
// UVolumeTexture creation (UE 5.4-safe)
// -------------------------------------------------------------------------

UVolumeTexture* AVMVolumeManager::CreateVolumeTextureFromRaw(const FVMNRRDHeader& Header, const TArray<uint8>& RawBytes)
{
	if (Header.SizeX <= 0 || Header.SizeY <= 0 || Header.SizeZ <= 0)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Invalid volume sizes: %d x %d x %d"), Header.SizeX, Header.SizeY, Header.SizeZ);
		return nullptr;
	}

	if (RawBytes.Num() <= 0)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Empty RAW data."));
		return nullptr;
	}

	// Create transient VolumeTexture
	UVolumeTexture* VolumeTex = NewObject<UVolumeTexture>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!VolumeTex)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Failed to allocate UVolumeTexture."));
		return nullptr;
	}

	// Setup basic flags
	VolumeTex->bNotOfflineProcessed = true;
	VolumeTex->MipGenSettings = TMGS_NoMipmaps;
	VolumeTex->SRGB = false;
	VolumeTex->CompressionSettings = TC_Default;
	VolumeTex->NeverStream = true;
	VolumeTex->Filter = TF_Bilinear;

	// Initialize the source with one G16 mip
	VolumeTex->Source.Init(Header.SizeX, Header.SizeY, Header.SizeZ,
		/*NumSlices=*/1, TSF_G16);

	uint8* DestData = VolumeTex->Source.LockMip(0);
	if (!DestData)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Failed to lock VolumeTexture mip 0."));
		return nullptr;
	}

	const int64 ExpectedBytes =
		static_cast<int64>(Header.SizeX) * static_cast<int64>(Header.SizeY) * static_cast<int64>(Header.SizeZ) * sizeof(uint16);

	const int64 CopyBytes = FMath::Min<int64>(ExpectedBytes, RawBytes.Num());
	if (CopyBytes > 0)
	{
		FMemory::Memcpy(DestData, RawBytes.GetData(), static_cast<SIZE_T>(CopyBytes));
	}

	// Zero-fill any remainder
	if (CopyBytes < ExpectedBytes)
	{
		const int64 Remaining = ExpectedBytes - CopyBytes;
		FMemory::Memset(DestData + CopyBytes, 0, static_cast<SIZE_T>(Remaining));
		UE_LOG(LogVMVolumeManager, Warning, TEXT("CreateVolumeTextureFromRaw: zero-filled %lld trailing bytes."), Remaining);
	}

	VolumeTex->Source.UnlockMip(0);

	// Create RHI resource
	VolumeTex->UpdateResource();

	return VolumeTex;
}

// -------------------------------------------------------------------------
// Hook into Raymarcher (no internals touched)
// -------------------------------------------------------------------------

void AVMVolumeManager::ApplyToRaymarchVolume(UVolumeTexture* VolumeTexture, const FVMNRRDHeader& Header)
{
	if (!TargetRaymarchVolume)
	{
		UE_LOG(LogVMVolumeManager, Warning,
			TEXT("ApplyToRaymarchVolume: TargetRaymarchVolume is null. "
				 "Use GetLoadedVolumeTexture() in BP to wire into RaymarchResources."));
		return;
	}

	if (!VolumeTexture)
	{
		UE_LOG(LogVMVolumeManager, Warning, TEXT("ApplyToRaymarchVolume: VolumeTexture is null."));
		return;
	}

	// We deliberately do NOT touch FBasicRaymarchRenderingResources here because
	// your RaymarchTypes.h has changed and field names are not stable.
	// Instead, you will:
	//   - In Blueprint, read GetLoadedVolumeTexture()
	//   - Use 'Set Members in BasicRaymarchRenderingResources' on RaymarchResources
	//   - Assign the correct field (e.g. DataVolumeTextureRef / IntensityTexture / etc.)
	//   - Then call TargetRaymarchVolume->SetAllMaterialParameters() from Blueprint.

	UE_LOG(LogVMVolumeManager, Log,
		TEXT("ApplyToRaymarchVolume: Texture '%s' ready (%dx%dx%d, min=%d, max=%d). "
			 "Wire it in Blueprint via GetLoadedVolumeTexture()."),
		*VolumeTexture->GetName(), Header.SizeX, Header.SizeY, Header.SizeZ, Header.MinValue, Header.MaxValue);
}

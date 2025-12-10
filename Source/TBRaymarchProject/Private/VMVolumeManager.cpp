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
#include "TextureUtilities.h"
#include "VolumeAsset/VolumeAsset.h"
#include "VolumeAsset/VolumeInfo.h"

DEFINE_LOG_CATEGORY_STATIC(LogVMVolumeManager, Log, All);

AVMVolumeManager::AVMVolumeManager()
{
	PrimaryActorTick.bCanEverTick = false;
	bVolumeLoadedSuccessfully = false;
	LoadedVolumeTexture = nullptr;
	LoadedVolumeAsset = nullptr;
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
	LoadedVolumeAsset = nullptr;
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

	UE_LOG(LogVMVolumeManager, Log, TEXT("Created VolumeTexture %s (%d, %d, %d)"), *VolumeTex->GetName(), VolumeTex->GetSizeX(),
		VolumeTex->GetSizeY(), VolumeTex->GetSizeZ());

	UVolumeAsset* VolumeAsset = BuildTransientVolumeAsset(Header, VolumeTex);
	if (!VolumeAsset)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Failed to wrap volume texture in VolumeAsset for: %s"), *AbsHeaderPath);
		return;
	}

	LoadedVolumeTexture = VolumeTex;
	LoadedVolumeAsset = VolumeAsset;
	LastHeader = Header;
	bVolumeLoadedSuccessfully = true;

	ApplyToRaymarchVolume(VolumeTex, VolumeAsset, Header);

	UE_LOG(LogVMVolumeManager, Log,
		TEXT("Loaded NRRD '%s' -> %dx%dx%d, spacing=%s mm, min=%d, max=%d"), *AbsHeaderPath, Header.SizeX, Header.SizeY,
		Header.SizeZ, *Header.Spacing.ToString(), Header.MinValue, Header.MaxValue);
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
	FVector SpaceDirs[3];
	bool bHasSpaceDirs = false;
	FVector Origin = FVector::ZeroVector;
	bool bHasOrigin = false;

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
		else if (Line.StartsWith(TEXT("space directions:")))
		{
			const FString Value = Line.Mid(16).TrimStartAndEnd();
			TArray<FString> Tokens;
			Value.ParseIntoArray(Tokens, TEXT(" "), true);

			int32 DirIndex = 0;
			for (const FString& Token : Tokens)
			{
				if (DirIndex >= 3)
				{
					break;
				}

				FString Clean = Token;
				Clean.ReplaceInline(TEXT("("), TEXT(""));
				Clean.ReplaceInline(TEXT(")"), TEXT(""));

				TArray<FString> Components;
				Clean.ParseIntoArray(Components, TEXT(","), true);

				if (Components.Num() == 3)
				{
					const float X = FCString::Atof(*Components[0]);
					const float Y = FCString::Atof(*Components[1]);
					const float Z = FCString::Atof(*Components[2]);
					SpaceDirs[DirIndex] = FVector(X, Y, Z);
					DirIndex++;
				}
			}

			bHasSpaceDirs = (DirIndex == 3);
		}
		else if (Line.StartsWith(TEXT("space origin:")))
		{
			const FString Value = Line.Mid(13).TrimStartAndEnd();
			FString Clean = Value;
			Clean.ReplaceInline(TEXT("("), TEXT(""));
			Clean.ReplaceInline(TEXT(")"), TEXT(""));

			TArray<FString> Components;
			Clean.ParseIntoArray(Components, TEXT(","), true);

			if (Components.Num() == 3)
			{
				Origin.X = FCString::Atof(*Components[0]);
				Origin.Y = FCString::Atof(*Components[1]);
				Origin.Z = FCString::Atof(*Components[2]);
				bHasOrigin = true;
			}
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
	if (bHasSpaceDirs)
	{
		// NRRD order: dirs[0]=axis 0 (Z), dirs[1]=axis 1 (Y), dirs[2]=axis 2 (X).
		OutHeader.Spacing = FVector(SpaceDirs[2].Size(), SpaceDirs[1].Size(), SpaceDirs[0].Size());
	}
	if (bHasOrigin)
	{
		OutHeader.Origin = Origin;
	}

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

	// Handle endian swap if needed (NRRD provides endianness info).
	if (!InOutHeader.bLittleEndian && PLATFORM_LITTLE_ENDIAN)
	{
		for (int32 i = 0; i < OutRawBytes.Num(); i += 2)
		{
			uint8 Temp = OutRawBytes[i];
			OutRawBytes[i] = OutRawBytes[i + 1];
			OutRawBytes[i + 1] = Temp;
		}
	}

	DataPtr = reinterpret_cast<const int16*>(OutRawBytes.GetData());

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

UVolumeTexture* AVMVolumeManager::CreateVolumeTextureFromRaw(FVMNRRDHeader& Header, const TArray<uint8>& RawBytes)
{
	if (Header.SizeX <= 0 || Header.SizeY <= 0 || Header.SizeZ <= 0)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("Invalid volume sizes: %d x %d x %d"), Header.SizeX, Header.SizeY, Header.SizeZ);
		return nullptr;
	}

	const int64 NumVoxels = static_cast<int64>(Header.SizeX) * Header.SizeY * Header.SizeZ;
	const int64 ExpectedBytes = NumVoxels * sizeof(int16);

	if (RawBytes.Num() < ExpectedBytes)
	{
		UE_LOG(LogVMVolumeManager, Error,
			TEXT("CreateVolumeTextureFromRaw: RawBytes too small. Have %d, expected at least %lld"), RawBytes.Num(), ExpectedBytes);
		return nullptr;
	}

	// Create transient volume texture directly from raw int16 data.
	UVolumeTexture* VolumeTex = nullptr;
	const bool bCreated = UVolumeTextureToolkit::CreateVolumeTextureTransient(
		VolumeTex, PF_G16, FIntVector(Header.SizeX, Header.SizeY, Header.SizeZ), const_cast<uint8*>(RawBytes.GetData()), true);

	if (!bCreated || !VolumeTex)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("CreateVolumeTextureFromRaw: CreateVolumeTextureTransient failed"));
		return nullptr;
	}

	VolumeTex->SRGB = false;
	VolumeTex->Filter = TF_Bilinear;
	VolumeTex->MipGenSettings = TMGS_NoMipmaps;
	VolumeTex->CompressionSettings = TC_Default;

	VolumeTex->UpdateResource();

	UE_LOG(LogVMVolumeManager, Log, TEXT("CreateVolumeTextureFromRaw: Created PF_G16 VolumeTexture %dx%dx%d"), Header.SizeX,
		Header.SizeY, Header.SizeZ);

	return VolumeTex;
}

// -------------------------------------------------------------------------
// Build a transient VolumeAsset so we can reuse the plugin's init path
// -------------------------------------------------------------------------

UVolumeAsset* AVMVolumeManager::BuildTransientVolumeAsset(const FVMNRRDHeader& Header, UVolumeTexture* VolumeTexture) const
{
	if (!VolumeTexture)
	{
		return nullptr;
	}

	UVolumeAsset* VolumeAsset = UVolumeAsset::CreateTransient(TEXT("VMRuntimeVolume"));
	if (!VolumeAsset)
	{
		return nullptr;
	}

	FVolumeInfo Info;
	Info.bParseWasSuccessful = true;
	Info.DataFileName = FPaths::GetCleanFilename(Header.RawFilePath);
	Info.OriginalFormat = EVolumeVoxelFormat::SignedShort;
	Info.ActualFormat = EVolumeVoxelFormat::SignedShort;
	Info.Dimensions = FIntVector(Header.SizeX, Header.SizeY, Header.SizeZ);
	Info.Spacing = Header.Spacing;
	Info.WorldDimensions =
		FVector(Header.Spacing.X * Header.SizeX, Header.Spacing.Y * Header.SizeY, Header.Spacing.Z * Header.SizeZ);
	Info.bIsNormalized = false;
	Info.MinValue = Header.MinValue;
	Info.MaxValue = Header.MaxValue;
	Info.BytesPerVoxel = FVolumeInfo::VoxelFormatByteSize(Info.OriginalFormat);
	Info.bIsSigned = FVolumeInfo::IsVoxelFormatSigned(Info.OriginalFormat);
	Info.DefaultWindowingParameters.Center = (static_cast<float>(Header.MinValue) + static_cast<float>(Header.MaxValue)) * 0.5f;
	Info.DefaultWindowingParameters.Width =
		FMath::Max(1.0f, static_cast<float>(Header.MaxValue - Header.MinValue));
	Info.DefaultWindowingParameters.LowCutoff = true;
	Info.DefaultWindowingParameters.HighCutoff = true;

	VolumeAsset->DataTexture = VolumeTexture;
	VolumeAsset->ImageInfo = Info;
	if (TransferFunctionOverride)
	{
		VolumeAsset->TransferFuncCurve = TransferFunctionOverride;
	}
	else
	{
		VolumeAsset->TransferFuncCurve = nullptr;	// Let RaymarchVolume create default TF texture
	}

	return VolumeAsset;
}

// -------------------------------------------------------------------------
// Hook into Raymarcher (uses plugin's public API)
// -------------------------------------------------------------------------

void AVMVolumeManager::ApplyToRaymarchVolume(UVolumeTexture* VolumeTexture, UVolumeAsset* VolumeAsset,
	const FVMNRRDHeader& Header)
{
	if (!TargetRaymarchVolume)
	{
		UE_LOG(LogVMVolumeManager, Warning,
			TEXT("ApplyToRaymarchVolume: TargetRaymarchVolume is null. Assign it in BP to see the volume."));
		return;
	}

	if (!VolumeTexture || !VolumeAsset)
	{
		UE_LOG(LogVMVolumeManager, Warning, TEXT("ApplyToRaymarchVolume: VolumeTexture or VolumeAsset is null."));
		return;
	}

	const bool bSet = TargetRaymarchVolume->SetVolumeAsset(VolumeAsset);
	if (!bSet)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("RaymarchVolume rejected VolumeAsset for '%s'"), *NRRDPath);
		return;
	}

	// RaymarchVolume::SetVolumeAsset already initializes resources and sets materials.
	TargetRaymarchVolume->SetAllMaterialParameters();
	TargetRaymarchVolume->SetMaterialWindowingParameters();

	UE_LOG(LogVMVolumeManager, Log,
		TEXT("RaymarchVolume bound volume '%s' (%dx%dx%d). Window center=%.2f width=%.2f"), *VolumeTexture->GetName(),
		Header.SizeX, Header.SizeY, Header.SizeZ, VolumeAsset->ImageInfo.DefaultWindowingParameters.Center,
		VolumeAsset->ImageInfo.DefaultWindowingParameters.Width);
}

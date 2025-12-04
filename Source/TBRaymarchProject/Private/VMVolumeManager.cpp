// VMVolumeManager.cpp
// Runtime loader for NRRD (intensity) volumes used by VoluMatrix.
//
// Key assumptions (must match dicom_to_nrrd.py):
//  - type: short        -> 16-bit signed integer (HU values)
//  - encoding: raw      -> no compression
//  - endian: little     -> little-endian (Windows default)
//  - dimension: 3
//  - sizes: Z Y X       -> we remap to X/Y/Z for Unreal volume texture
//  - no "spacings" field when "space directions" is present (Slicer compatibility)

#include "VMVolumeManager.h"

#include "Engine/Texture.h"
#include "Engine/VolumeTexture.h"
#include "HAL/PlatformFilemanager.h"
#include "Logging/LogMacros.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// Raymarcher plugin
#include "Actor/RaymarchVolume.h"

DEFINE_LOG_CATEGORY_STATIC(LogVMVolumeManager, Log, All);

AVMVolumeManager::AVMVolumeManager()
{
	PrimaryActorTick.bCanEverTick = false;
	LoadedIntensityTexture = nullptr;
	TargetRaymarchVolume = nullptr;
}

void AVMVolumeManager::BeginPlay()
{
	Super::BeginPlay();

	// Optional: auto-load on BeginPlay if NrrdHeaderPath is set.
	if (!NrrdHeaderPath.IsEmpty())
	{
		UE_LOG(LogVMVolumeManager, Log, TEXT("BeginPlay: attempting auto-load of NRRD '%s'"), *NrrdHeaderPath);

		FVMNRRDHeaderInfo HeaderInfo;
		UVolumeTexture* Tex = LoadNRRDIntensity(NrrdHeaderPath, HeaderInfo);

		if (Tex)
		{
			LoadedIntensityTexture = Tex;
			LastHeaderInfo = HeaderInfo;
			UE_LOG(LogVMVolumeManager, Log, TEXT("BeginPlay: successfully loaded NRRD (%dx%dx%d)"), HeaderInfo.SizeX,
				HeaderInfo.SizeY, HeaderInfo.SizeZ);

			if (TargetRaymarchVolume)
			{
				ApplyIntensityToRaymarcher(TargetRaymarchVolume, Tex);
			}
		}
	}
}

void AVMVolumeManager::LoadAndApplyNRRD()
{
	FVMNRRDHeaderInfo HeaderInfo;
	UVolumeTexture* Tex = LoadNRRDIntensity(NrrdHeaderPath, HeaderInfo);

	if (!Tex)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("LoadAndApplyNRRD: failed to load '%s'"), *NrrdHeaderPath);
		return;
	}

	LoadedIntensityTexture = Tex;
	LastHeaderInfo = HeaderInfo;

	UE_LOG(LogVMVolumeManager, Log, TEXT("LoadAndApplyNRRD: loaded NRRD (%dx%dx%d) from '%s'"), HeaderInfo.SizeX, HeaderInfo.SizeY,
		HeaderInfo.SizeZ, *NrrdHeaderPath);

	if (TargetRaymarchVolume)
	{
		ApplyIntensityToRaymarcher(TargetRaymarchVolume, Tex);
	}
}

// ---- Static helpers ----

UVolumeTexture* AVMVolumeManager::LoadNRRDIntensity(const FString& InNrrdHeaderPath, FVMNRRDHeaderInfo& OutHeaderInfo)
{
	FString ErrorMsg;

	if (InNrrdHeaderPath.IsEmpty())
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("LoadNRRDIntensity: NRRD header path is empty"));
		return nullptr;
	}

	FString NormalizedPath = InNrrdHeaderPath;
	FPaths::NormalizeFilename(NormalizedPath);

	UE_LOG(LogVMVolumeManager, Log, TEXT("LoadNRRDIntensity: loading NRRD header '%s'"), *NormalizedPath);

	FVMNRRDHeaderInfo Header;
	if (!ParseNRRDHeader(NormalizedPath, Header, ErrorMsg))
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("LoadNRRDIntensity: header parse failed: %s"), *ErrorMsg);
		return nullptr;
	}

	TArray<uint8> RawBytes;
	if (!LoadRawData(NormalizedPath, Header, RawBytes, ErrorMsg))
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("LoadNRRDIntensity: raw data load failed: %s"), *ErrorMsg);
		return nullptr;
	}

	// Basic sanity: check size matches
	const int64 ExpectedSamples =
		static_cast<int64>(Header.SizeX) * static_cast<int64>(Header.SizeY) * static_cast<int64>(Header.SizeZ);

	const int64 ExpectedBytes = ExpectedSamples * sizeof(int16);

	if (RawBytes.Num() != ExpectedBytes)
	{
		UE_LOG(LogVMVolumeManager, Warning,
			TEXT("LoadNRRDIntensity: raw size mismatch. Expected %lld bytes, got %d. "
				 "Continuing, but data might be truncated or padded."),
			ExpectedBytes, RawBytes.Num());
	}

	int16 MinVal = 0;
	int16 MaxVal = 0;
	ComputeVolumeMinMaxInt16(RawBytes, MinVal, MaxVal);

	UE_LOG(LogVMVolumeManager, Log, TEXT("LoadNRRDIntensity: volume min/max (int16) = [%d, %d]"), MinVal, MaxVal);

	UVolumeTexture* VolumeTex = CreateVolumeTextureFromRaw16(Header, RawBytes);
	if (!VolumeTex)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("LoadNRRDIntensity: failed to create UVolumeTexture"));
		return nullptr;
	}

	OutHeaderInfo = Header;
	return VolumeTex;
}

void AVMVolumeManager::ApplyIntensityToRaymarcher(ARaymarchVolume* RaymarchVolume, UVolumeTexture* IntensityTexture)
{
	if (!RaymarchVolume)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("ApplyIntensityToRaymarcher: RaymarchVolume is null"));
		return;
	}
	if (!IntensityTexture)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("ApplyIntensityToRaymarcher: IntensityTexture is null"));
		return;
	}

	// IMPORTANT:
	// We deliberately avoid touching RaymarchResources internals here, because the struct layout
	// changed between plugin versions and your project has already customized it once.
	//
	// Instead, you should:
	//   - Expose RaymarchResources in Blueprint,
	//   - Use "Set Members in BasicRaymarchRenderingResources",
	//   - Assign the IntensityTexture to the appropriate field (e.g. DataVolumeTextureRef),
	//   - Then call RaymarchVolume->SetAllMaterialParameters() from Blueprint (if needed).
	//
	// This function is kept as a placeholder / future C++ hook if we later standardize an API.

	UE_LOG(LogVMVolumeManager, Log, TEXT("ApplyIntensityToRaymarcher: called with RaymarchVolume='%s', Texture='%s'"),
		*RaymarchVolume->GetName(), *IntensityTexture->GetName());
}

// ------------------- Internal implementation -------------------

bool AVMVolumeManager::ParseNRRDHeader(const FString& NhdrFilePath, FVMNRRDHeaderInfo& OutHeader, FString& OutError)
{
	OutError.Reset();

	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *NhdrFilePath))
	{
		OutError = FString::Printf(TEXT("Could not read header file '%s'"), *NhdrFilePath);
		return false;
	}

	if (Lines.Num() == 0)
	{
		OutError = TEXT("Header file is empty");
		return false;
	}

	// First line must be NRRD magic
	{
		const FString& FirstLine = Lines[0].TrimStartAndEnd();
		if (!FirstLine.StartsWith(TEXT("NRRD")))
		{
			OutError = FString::Printf(TEXT("Invalid NRRD magic line: '%s'"), *FirstLine);
			return false;
		}
	}

	int32 Dim = 0;
	int32 SizesTokens[3] = {0, 0, 0};

	for (const FString& RawLine : Lines)
	{
		FString Line = RawLine.TrimStartAndEnd();

		// Skip comments and blank lines
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
		{
			continue;
		}

		// dimension:
		if (Line.StartsWith(TEXT("dimension:")))
		{
			FString Right = Line.Mid(10).TrimStartAndEnd();
			Dim = FCString::Atoi(*Right);
		}
		// sizes:
		else if (Line.StartsWith(TEXT("sizes:")))
		{
			FString Right = Line.Mid(6).TrimStartAndEnd();
			TArray<FString> Tokens;
			Right.ParseIntoArray(Tokens, TEXT(" "), true);

			if (Tokens.Num() < 3)
			{
				OutError = FString::Printf(TEXT("sizes line has <3 tokens: '%s'"), *Right);
				return false;
			}

			// Converter currently writes: sizes: Z Y X
			SizesTokens[0] = FCString::Atoi(*Tokens[0]);	// Z
			SizesTokens[1] = FCString::Atoi(*Tokens[1]);	// Y
			SizesTokens[2] = FCString::Atoi(*Tokens[2]);	// X
		}
		// type:
		else if (Line.StartsWith(TEXT("type:")))
		{
			OutHeader.Type = Line.Mid(5).TrimStartAndEnd();
		}
		// encoding:
		else if (Line.StartsWith(TEXT("encoding:")))
		{
			OutHeader.Encoding = Line.Mid(9).TrimStartAndEnd();
		}
		// endian:
		else if (Line.StartsWith(TEXT("endian:")))
		{
			OutHeader.Endian = Line.Mid(7).TrimStartAndEnd();
		}
		// data file:
		else if (Line.StartsWith(TEXT("data file:")))
		{
			OutHeader.DataFileName = Line.Mid(10).TrimStartAndEnd();
		}
		// spacings: (optional, Slicer-sensitive)
		else if (Line.StartsWith(TEXT("spacings:")))
		{
			FString Right = Line.Mid(9).TrimStartAndEnd();
			TArray<FString> Tokens;
			Right.ParseIntoArray(Tokens, TEXT(" "), true);
			if (Tokens.Num() >= 3)
			{
				OutHeader.Spacing.X = FCString::Atof(*Tokens[2]);	 // X
				OutHeader.Spacing.Y = FCString::Atof(*Tokens[1]);	 // Y
				OutHeader.Spacing.Z = FCString::Atof(*Tokens[0]);	 // Z
			}
		}
	}

	if (Dim != 3)
	{
		OutError = FString::Printf(TEXT("Expected dimension=3, got %d"), Dim);
		return false;
	}

	if (SizesTokens[0] <= 0 || SizesTokens[1] <= 0 || SizesTokens[2] <= 0)
	{
		OutError = TEXT("Invalid sizes in header (non-positive)");
		return false;
	}

	// Map from NRRD order (Z Y X) to Unreal VolumeTexture order (X Y Z)
	OutHeader.SizeZ = SizesTokens[0];
	OutHeader.SizeY = SizesTokens[1];
	OutHeader.SizeX = SizesTokens[2];

	if (!OutHeader.Type.Equals(TEXT("short"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogVMVolumeManager, Warning,
			TEXT("ParseNRRDHeader: type '%s' is not 'short'. VoluMatrix loader currently assumes 16-bit signed data."),
			*OutHeader.Type);
	}

	if (!OutHeader.Encoding.Equals(TEXT("raw"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(TEXT("Unsupported encoding '%s' (only 'raw' is supported)."), *OutHeader.Encoding);
		return false;
	}

	if (!OutHeader.Endian.IsEmpty() && !OutHeader.Endian.Equals(TEXT("little"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(TEXT("Unsupported endian '%s' (only 'little' is supported)."), *OutHeader.Endian);
		return false;
	}

	if (OutHeader.DataFileName.IsEmpty())
	{
		OutError = TEXT("data file field is missing in header");
		return false;
	}

	return true;
}

bool AVMVolumeManager::LoadRawData(
	const FString& NhdrFilePath, const FVMNRRDHeaderInfo& Header, TArray<uint8>& OutBytes, FString& OutError)
{
	OutError.Reset();
	OutBytes.Reset();

	// If DataFileName is relative, use the header directory as base.
	const FString HeaderDir = FPaths::GetPath(NhdrFilePath);

	FString RawPath = Header.DataFileName;
	FPaths::NormalizeFilename(RawPath);

	if (!FPaths::IsRelative(RawPath))
	{
		// Absolute path as written in header
	}
	else
	{
		RawPath = FPaths::Combine(HeaderDir, RawPath);
	}

	FPaths::NormalizeFilename(RawPath);

	if (!FPaths::FileExists(RawPath))
	{
		OutError = FString::Printf(TEXT("RAW file '%s' does not exist"), *RawPath);
		return false;
	}

	if (!FFileHelper::LoadFileToArray(OutBytes, *RawPath))
	{
		OutError = FString::Printf(TEXT("Failed to read RAW file '%s'"), *RawPath);
		return false;
	}

	UE_LOG(LogVMVolumeManager, Log, TEXT("LoadRawData: loaded %d bytes from '%s'"), OutBytes.Num(), *RawPath);

	return true;
}

UVolumeTexture* AVMVolumeManager::CreateVolumeTextureFromRaw16(const FVMNRRDHeaderInfo& Header, const TArray<uint8>& Bytes)
{
	if (Header.SizeX <= 0 || Header.SizeY <= 0 || Header.SizeZ <= 0)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("CreateVolumeTextureFromRaw16: invalid dimensions (%d, %d, %d)"), Header.SizeX,
			Header.SizeY, Header.SizeZ);
		return nullptr;
	}

	// Create transient UVolumeTexture
	UVolumeTexture* VolumeTex = NewObject<UVolumeTexture>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!VolumeTex)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("CreateVolumeTextureFromRaw16: failed to allocate UVolumeTexture"));
		return nullptr;
	}

	VolumeTex->bNotOfflineProcessed = true;
	VolumeTex->CompressionSettings = TC_Default;
	VolumeTex->MipGenSettings = TMGS_NoMipmaps;
	VolumeTex->SRGB = false;
	VolumeTex->Filter = TF_Bilinear;
	VolumeTex->NeverStream = true;

	// Initialize source with G16 format mip.
	VolumeTex->Source.Init(Header.SizeX, Header.SizeY, Header.SizeZ,
		/*NumSlices*/ 1, TSF_G16);

	// Copy bytes into the source mip
	uint8* DestData = VolumeTex->Source.LockMip(0);
	if (!DestData)
	{
		UE_LOG(LogVMVolumeManager, Error, TEXT("CreateVolumeTextureFromRaw16: failed to lock mip 0"));
		return nullptr;
	}

	const int64 ExpectedSamples =
		static_cast<int64>(Header.SizeX) * static_cast<int64>(Header.SizeY) * static_cast<int64>(Header.SizeZ);

	const int64 ExpectedBytes = ExpectedSamples * sizeof(int16);
	const int64 CopyBytes = FMath::Min<int64>(ExpectedBytes, Bytes.Num());

	if (CopyBytes > 0)
	{
		FMemory::Memcpy(DestData, Bytes.GetData(), static_cast<SIZE_T>(CopyBytes));
	}

	// Zero-fill any remaining region if the file was shorter than expected.
	if (CopyBytes < ExpectedBytes)
	{
		const int64 Remaining = ExpectedBytes - CopyBytes;
		FMemory::Memset(DestData + CopyBytes, 0, static_cast<SIZE_T>(Remaining));

		UE_LOG(LogVMVolumeManager, Warning, TEXT("CreateVolumeTextureFromRaw16: RAW shorter than expected, zero-filled %lld bytes"),
			Remaining);
	}

	VolumeTex->Source.UnlockMip(0);

	// Actually allocate GPU resources.
	VolumeTex->UpdateResource();

	UE_LOG(LogVMVolumeManager, Log, TEXT("CreateVolumeTextureFromRaw16: created UVolumeTexture (%dx%dx%d)"), Header.SizeX,
		Header.SizeY, Header.SizeZ);

	return VolumeTex;
}

void AVMVolumeManager::ComputeVolumeMinMaxInt16(const TArray<uint8>& Bytes, int16& OutMin, int16& OutMax)
{
	OutMin = 0;
	OutMax = 0;

	const int64 TotalBytes = Bytes.Num();
	if (TotalBytes < static_cast<int64>(sizeof(int16)))
	{
		return;
	}

	const int64 NumSamples = TotalBytes / sizeof(int16);
	const int16* DataPtr = reinterpret_cast<const int16*>(Bytes.GetData());

	int16 LocalMin = DataPtr[0];
	int16 LocalMax = DataPtr[0];

	for (int64 i = 1; i < NumSamples; ++i)
	{
		const int16 V = DataPtr[i];
		if (V < LocalMin)
			LocalMin = V;
		if (V > LocalMax)
			LocalMax = V;
	}

	OutMin = LocalMin;
	OutMax = LocalMax;
}

# VoluMatrix – Medical Volume Rendering in Unreal Engine

**VoluMatrix** is a medical volume visualization project built on top of  
[Tommy Pazar’s TBRaymarcherPlugin](https://github.com/tommybazar/TBRaymarcherPlugin).

The goal is a clean, production-oriented pipeline:

> **DICOM → ITK (NRRD) → Unreal Engine (TBRaymarcher) → VR (Quest 3)**

It is intended as a **beginner-friendly starting point** for:

- Loading DICOM series with ITK.
- Exporting NRRD (NHDR + RAW).
- Visualizing volumes in Unreal using TBRaymarcher.
- Interacting with the volume in VR (Quest 3).

---

To clone this repo with the plugin included, use this command:

$ git clone --recurse-submodules https://github.com/rajwardhan1920/Volumatrix.git

Or after a regular clone, perform a

$ git lfs pull 

$ git submodule init

$ git submodule update


# 4. Use a UE 5.4-compatible Visual Studio toolchain

UE 5.4 is picky about MSVC/SDK combos. If you only have very new MSVC (14.44+), UBT can fail.

Install these via Visual Studio Installer:

MSVC v143 build tools (specifically 14.38.x or 14.39.x side-by-side)

Windows 10/11 SDK 10.0.22621.0 (or at least 10.0.22000+) 
Reddit

Then in VS Installer:
Individual components → check

MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.38/14.39)

Windows 11 SDK (10.0.22621.0)

# You don’t need to uninstall 14.44; just add the older v143 toolset so UE can select it.







## 1. Architecture Overview

```text
                   ┌─────────────────────────┐
                   │         Doctor          │
                   │    (Radiology Dept)     │
                   └───────────┬─────────────┘
                               │
                               │  DICOM Series (.dcm)
                               ▼
           ┌────────────────────────────────────────┐
           │      ITK Preprocessing Pipeline        │
           │  (Standalone C++/Python Tool)          │
           ├────────────────────────────────────────┤
           │ • Read DICOM (ITK + GDCM)              │
           │ • Sort slices                          │
           │ • Rescale HU (slope/intercept)         │
           │ • Extract spacing/orientation          │
           │ • Build 3D voxel grid                  │
           │ • Export:                              │
           │     - volume.raw  (UInt16 voxels)      │
           │     - volume.nhdr (NRRD metadata)      │
           └─────────────────────────┬──────────────┘
                                     │
                                     │  NRRD (NHDR + RAW)
                                     ▼
         ┌────────────────────────────────────────────┐
         │           Unreal Engine 5.6                │
         │     (TBRaymarcher-based Raymarcher)        │
         ├────────────────────────────────────────────┤
         │  NRRD Loader (C++)                         │
         │    • Parse NHDR                            │
         │    • Load RAW                              │
         │    • Create UVolumeTexture (PF_G16)        │
         ├────────────────────────────────────────────┤
         │  Raymarcher Renderer (HLSL/C++)            │
         │    • Volume sampling                       │
         │    • Transfer function                     │
         │    • Window/level presets                  │
         │    • Early ray termination                 │
         │    • Adaptive step size                    │
         ├────────────────────────────────────────────┤
         │  VR Interaction (Quest 3)                  │
         │    • Slice planes                          │
         │    • Clipping box                          │
         │    • Annotations                           │
         │    • Measurement tools                     │
         │    • Layer visibility                      │
         ├────────────────────────────────────────────┤
         │   Optional Systems (Roadmap)               │
         │    • Multiplayer sync                      │
         │    • Mesh extraction (Marching Cubes)      │
         │    • Timeline + history                    │
         │    • QA export                             │
         └─────────────────────────┬──────────────────┘
                                   │
                                   ▼
                         VR Output on Quest 3

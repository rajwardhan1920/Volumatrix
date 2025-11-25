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

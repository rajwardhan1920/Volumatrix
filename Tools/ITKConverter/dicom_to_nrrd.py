import os
import sys
import numpy as np
import itk


def load_dicom_series(dicom_dir: str):
    """
    Load the FIRST DICOM series from the given folder
    using ITK + GDCM. Returns a 3D ITK Image with signed short pixels.
    """
    pixel_type = itk.ctype("signed short")
    image_type = itk.Image[pixel_type, 3]

    reader = itk.ImageSeriesReader[image_type].New()
    names = itk.GDCMSeriesFileNames.New()
    names.SetDirectory(dicom_dir)

    series_uids = names.GetSeriesUIDs()
    if len(series_uids) == 0:
        raise RuntimeError(f"No DICOM series found in folder: {dicom_dir}")

    print("Found DICOM Series UIDs:")
    for i, uid in enumerate(series_uids):
        print(f"  [{i}] {uid}")

    chosen_uid = series_uids[0]
    file_list = names.GetFileNames(chosen_uid)
    print(f"Using series [0]: {chosen_uid} — {len(file_list)} slices")

    reader.SetFileNames(file_list)
    reader.Update()
    return reader.GetOutput()


def apply_hu_scaling(image):
    """
    Apply RescaleSlope and RescaleIntercept to convert raw DICOM
    values to CT Hounsfield Units (HU). Output is int16.
    """
    slope = 1.0
    intercept = 0.0

    # Try to read from DICOM meta
    try:
        meta = image.GetMetaDataDictionary()
        keys = meta.GetKeys()
        for key in keys:
            val = itk.template(meta)[1].GetMetaDataObjectValue(meta, key)
            if "RescaleSlope" in key:
                slope = float(val)
            if "RescaleIntercept" in key:
                intercept = float(val)
    except Exception as e:
        print("Warning: could not read RescaleSlope/Intercept:", e)

    # ITK → NumPy (shape: [z, y, x])
    arr = itk.GetArrayFromImage(image).astype(np.float32)

    # HU = raw * slope + intercept
    hu = arr * slope + intercept

    # Clip to a typical CT HU range and convert to int16
    hu = np.clip(hu, -1024, 3071).astype(np.int16)

    # NumPy → ITK (preserve orientation info)
    out = itk.GetImageFromArray(hu)
    out.CopyInformation(image)
    return out


def write_nrrd_raw(image, nhdr_path, raw_path):
    """
    Write a simple, Slicer-compatible NRRD+RAW pair.

    - No 'space', 'space directions', or 'spacings' fields.
    - Only essential header fields.
    - sizes are written as X Y Z, matching NumPy's [z, y, x] layout.
    """
    # ITK → NumPy
    arr = itk.GetArrayFromImage(image).astype(np.int16)
    nz, ny, nx = arr.shape  # [z, y, x]

    # Write raw data
    with open(raw_path, "wb") as f:
        arr.tofile(f)

    # NRRD header (minimal but valid)
    header_lines = [
        "NRRD0005",
        "# VoluMatrix intensity volume (simple, no orientation)",
        "type: short",
        "dimension: 3",
        # NRRD standard is sizes in X Y Z order
        f"sizes: {nx} {ny} {nz}",
        "encoding: raw",
        "endian: little",
        f"data file: {os.path.basename(raw_path)}",
    ]

    with open(nhdr_path, "w") as f:
        f.write("\n".join(header_lines) + "\n")

    print("\nWrote NRRD:")
    print(f"  nhdr: {nhdr_path}")
    print(f"  raw : {raw_path}")
    print(f"  sizes (X Y Z): {nx} {ny} {nz}")


def main():
    if len(sys.argv) != 3:
        print("Usage:")
        print("  python dicom_to_nrrd.py <dicom_folder> <output_base>")
        print("Example:")
        print(r"  python dicom_to_nrrd.py D:\Data\CT_01 .\output\patient1")
        sys.exit(1)

    dicom_dir = sys.argv[1]
    out_base = sys.argv[2]

    if not os.path.isdir(dicom_dir):
        raise RuntimeError(f"Invalid DICOM folder: {dicom_dir}")

    out_dir = os.path.dirname(out_base)
    if out_dir and not os.path.exists(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    print("=== Loading DICOM ===")
    img = load_dicom_series(dicom_dir)

    print("=== Applying HU scaling ===")
    img_hu = apply_hu_scaling(img)

    nhdr_path = out_base + ".nhdr"
    raw_path = out_base + ".raw"

    print("=== Writing NRRD ===")
    write_nrrd_raw(img_hu, nhdr_path, raw_path)

    print("=== DONE ===")


if __name__ == "__main__":
    main()

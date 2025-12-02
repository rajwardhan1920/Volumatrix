import os
import sys
import numpy as np
import itk


def load_dicom_series(dicom_dir: str):
    """
    Load the first valid DICOM series found in a folder.

    For now we just pick the first SeriesInstanceUID that GDCM finds.
    Later we can add an option to choose which series to export.
    """
    pixel_type = itk.ctype('signed short')
    image_type = itk.Image[pixel_type, 3]

    reader = itk.ImageSeriesReader[image_type].New()
    names_generator = itk.GDCMSeriesFileNames.New()
    names_generator.SetDirectory(dicom_dir)

    series_uids = names_generator.GetSeriesUIDs()
    if len(series_uids) == 0:
        raise RuntimeError(f"No DICOM series found in {dicom_dir}")

    print("Found series UIDs:")
    for idx, uid in enumerate(series_uids):
        print(f"  [{idx}] {uid}")

    # For now, just take the first series
    series_uid = series_uids[0]
    file_names = names_generator.GetFileNames(series_uid)
    print(f"Using series index 0, UID={series_uid}, number of slices={len(file_names)}")

    reader.SetFileNames(file_names)
    reader.Update()

    image = reader.GetOutput()
    return image


def apply_hu_scaling(image):
    """
    Apply HU = raw * slope + intercept if DICOM metadata has RescaleSlope/Intercept.
    Falls back to identity if not found.
    """
    slope = 1.0
    intercept = 0.0

    try:
        meta_dict = image.GetMetaDataDictionary()
        keys = list(meta_dict.GetKeys())
        for key in keys:
            if "RescaleSlope" in key:
                slope = float(itk.template(meta_dict)[1].GetMetaDataObjectValue(meta_dict, key))
            if "RescaleIntercept" in key:
                intercept = float(itk.template(meta_dict)[1].GetMetaDataObjectValue(meta_dict, key))
    except Exception as e:
        print("Warning: could not read RescaleSlope/Intercept, using defaults 1/0:", e)

    arr = itk.GetArrayFromImage(image).astype(np.float32)  # [z, y, x]
    hu = arr * slope + intercept

    # Clip to a sane CT range and cast to int16
    hu = np.clip(hu, -1024, 3071).astype(np.int16)

    hu_image = itk.GetImageFromArray(hu)  # back to ITK, still [z, y, x] logically
    hu_image.CopyInformation(image)
    return hu_image


def write_nrrd_raw(image, nhdr_path: str, raw_path: str):
    """
    Minimal, Slicer-friendly NRRD writer.

    - Uses the array exactly as GetArrayFromImage gives it: [z, y, x]
    - Writes sizes in the SAME order: sizes: Z Y X
    - Does not try to be clever with space directions yet.
    """
    # ITK -> numpy: shape [z, y, x]
    arr = itk.GetArrayFromImage(image)  # [z, y, x]
    arr = arr.astype(np.int16, copy=False)

    # Verify shape against ITK size
    size_x, size_y, size_z = image.GetLargestPossibleRegion().GetSize()  # ITK order: (x, y, z)
    if arr.shape != (size_z, size_y, size_x):
        print("Warning: array shape and ITK size mismatch:",
              "arr.shape =", arr.shape,
              "ITK size =", (size_z, size_y, size_x))

    # Write raw binary exactly in this [z, y, x] order
    with open(raw_path, "wb") as f:
        arr.tofile(f)

    # IMPORTANT: sizes MUST match the array order we wrote: Z Y X
    size_z, size_y, size_x = arr.shape  # note the order
    header = []
    header.append("NRRD0005")
    header.append("# VoluMatrix intensity volume (simple writer)")
    header.append("type: short")
    header.append("dimension: 3")
    header.append(f"sizes: {size_z} {size_y} {size_x}")
    header.append("encoding: raw")
    header.append("endian: little")

    # Simple axis definitions: index space, not real patient space yet
    header.append("space: right-anterior-superior")
    header.append("space directions: (1,0,0) (0,1,0) (0,0,1)")
    header.append("space origin: (0,0,0)")
    header.append(f"data file: {os.path.basename(raw_path)}")

    with open(nhdr_path, "w") as f:
        f.write("\n".join(header) + "\n")

    print("Wrote NRRD:")
    print("  nhdr:", nhdr_path)
    print("  raw :", raw_path)
    print("  sizes (Z Y X):", size_z, size_y, size_x)


def main():
    if len(sys.argv) != 3:
        print("Usage: python dicom_to_nrrd.py <dicom_folder> <output_basename>")
        print("Example: python dicom_to_nrrd.py \"D:\\VM REPO\\Volumatrix\\Dcm Files\\SampleDICOM\" ./output/patient1")
        sys.exit(1)

    dicom_dir = sys.argv[1]
    out_base = sys.argv[2]

    if not os.path.isdir(dicom_dir):
        raise RuntimeError(f"Not a directory: {dicom_dir}")

    out_dir = os.path.dirname(out_base)
    if out_dir and not os.path.exists(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    print(f"Loading DICOM series from: {dicom_dir}")
    image = load_dicom_series(dicom_dir)

    print("Applying HU scaling...")
    hu_image = apply_hu_scaling(image)

    nhdr_path = out_base + ".nhdr"
    raw_path = out_base + ".raw"

    print(f"Writing NRRD header: {nhdr_path}")
    print(f"Writing RAW data: {raw_path}")
    write_nrrd_raw(hu_image, nhdr_path, raw_path)

    print("Done.")


if __name__ == "__main__":
    main()

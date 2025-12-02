import os
import sys
import numpy as np
import itk


def load_dicom_series(dicom_dir: str):
    # ITK ImageSeriesReader will auto-detect series and sort by ImagePositionPatient/InstanceNumber
    pixel_type = itk.ctype('signed short')
    image_type = itk.Image[pixel_type, 3]

    reader = itk.ImageSeriesReader[image_type].New()
    names_generator = itk.GDCMSeriesFileNames.New()
    names_generator.SetDirectory(dicom_dir)

    series_uids = names_generator.GetSeriesUIDs()
    if len(series_uids) == 0:
        raise RuntimeError(f"No DICOM series found in {dicom_dir}")

    # For now, just take the first series
    series_uid = series_uids[0]
    file_names = names_generator.GetFileNames(series_uid)

    reader.SetFileNames(file_names)
    reader.Update()

    image = reader.GetOutput()
    return image


def apply_hu_scaling(image):
    # Read rescale slope/intercept from metadata if present
    # We assume all slices share the same values, so we check the first one
    # NOTE: If this fails, we just return the original image (not ideal, but safe fallback).
    meta_dict = image.GetMetaDataDictionary()
    slope = 1.0
    intercept = 0.0

    for key in meta_dict.GetKeys():
        if "RescaleSlope" in key:
            slope = float(itk.template(meta_dict)[1].GetMetaDataObjectValue(meta_dict, key))
        if "RescaleIntercept" in key:
            intercept = float(itk.template(meta_dict)[1].GetMetaDataObjectValue(meta_dict, key))

    # Convert to numpy, apply HU, then cast back to int16
    arr = itk.GetArrayFromImage(image).astype(np.float32)
    hu = arr * slope + intercept
    hu = np.clip(hu, -1024, 3071)  # reasonable CT brain range
    hu = hu.astype(np.int16)

    hu_image = itk.GetImageFromArray(hu)
    hu_image.CopyInformation(image)
    return hu_image


def write_nrrd_raw(image, nhdr_path: str, raw_path: str):
    # Convert to numpy
    arr = itk.GetArrayFromImage(image)  # shape: [z, y, x]
    # ITK's GetArrayFromImage gives z-fastest; NRRD default is dimension: 3, sizes: x y z
    # We'll write RAW in x-fastest order, so we must transpose.
    arr_for_nrrd = np.transpose(arr, (2, 1, 0))  # [x, y, z]
    arr_for_nrrd = arr_for_nrrd.astype(np.int16)

    # Write raw binary
    with open(raw_path, "wb") as f:
        arr_for_nrrd.tofile(f)

    # Header info
    size_x, size_y, size_z = arr_for_nrrd.shape
    spacing = image.GetSpacing()  # (sx, sy, sz)
    origin = image.GetOrigin()    # (ox, oy, oz)
    direction = image.GetDirection()  # 3x3 ITK direction cosine matrix

    # Convert direction to rows for NRRD "space directions"
    # ITK uses LPS; we'll declare NRRD space as "left-posterior-superior"
    dir_matrix = np.array(direction)
    sx = "({:.6f},{:.6f},{:.6f})".format(* (dir_matrix[:, 0] * spacing[0]))
    sy = "({:.6f},{:.6f},{:.6f})".format(* (dir_matrix[:, 1] * spacing[1]))
    sz = "({:.6f},{:.6f},{:.6f})".format(* (dir_matrix[:, 2] * spacing[2]))

    header = []
    header.append("NRRD0005")
    header.append("# VoluMatrix intensity volume")
    header.append("type: short")
    header.append("dimension: 3")
    header.append(f"sizes: {size_x} {size_y} {size_z}")
    header.append("encoding: raw")
    header.append("endian: little")
    header.append("space: left-posterior-superior")
    header.append(f"space origin: ({origin[0]:.6f},{origin[1]:.6f},{origin[2]:.6f})")
    header.append(f"space directions: {sx} {sy} {sz}")
    header.append(f"data file: {os.path.basename(raw_path)}")

    with open(nhdr_path, "w") as f:
        f.write("\n".join(header) + "\n")


def main():
    if len(sys.argv) != 3:
        print("Usage: python dicom_to_nrrd.py <dicom_folder> <output_basename>")
        print("Example: python dicom_to_nrrd.py ./SampleDICOM ./output/intensity")
        sys.exit(1)

    dicom_dir = sys.argv[1]
    out_base = sys.argv[2]

    if not os.path.isdir(dicom_dir):
        raise RuntimeError(f"Not a directory: {dicom_dir}")

    os.makedirs(os.path.dirname(out_base), exist_ok=True)

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

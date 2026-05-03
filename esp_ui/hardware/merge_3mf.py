#!/usr/bin/env python3
"""Merge two OpenSCAD 3MF files into one with multiple objects.

OpenSCAD's 3MF export writes one object per file. Slicers like PrusaSlicer,
OrcaSlicer, and Bambu Studio all support 3MF with multiple objects, treating
each as a separate part that can be assigned its own filament. This script
combines two 3MFs into that form.

Usage:
    python merge_3mf.py output.3mf input1.3mf input2.3mf [name1] [name2]
"""
import sys
import zipfile
import xml.etree.ElementTree as ET

NS = "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"


def merge(output_path, input_paths, names=None):
    objects = []
    items = []
    next_id = 1

    for idx, src in enumerate(input_paths):
        with zipfile.ZipFile(src, "r") as zf:
            with zf.open("3D/3dmodel.model") as f:
                tree = ET.parse(f)
        root = tree.getroot()
        # Strip the OpenSCAD-emitted namespace prefix off element tags so we
        # can re-emit cleanly under the default namespace.
        for el in root.iter():
            if el.tag.startswith(f"{{{NS}}}"):
                el.tag = el.tag[len(NS) + 2:]

        resources = root.find("resources")
        build = root.find("build")
        id_map = {}
        for obj in resources.findall("object"):
            old_id = obj.get("id")
            new_id = str(next_id)
            next_id += 1
            id_map[old_id] = new_id
            obj.set("id", new_id)
            if names and idx < len(names):
                obj.set("name", names[idx])
            objects.append(obj)
        for item in build.findall("item"):
            old = item.get("objectid")
            item.set("objectid", id_map[old])
            items.append(item)

    ET.register_namespace("", NS)
    new_root = ET.Element(
        f"{{{NS}}}model",
        attrib={"unit": "millimeter", "{http://www.w3.org/XML/1998/namespace}lang": "en-US"},
    )
    res_el = ET.SubElement(new_root, f"{{{NS}}}resources")
    for obj in objects:
        # Re-tag the descendants under the namespace so ET serialises correctly.
        for el in obj.iter():
            if not el.tag.startswith("{"):
                el.tag = f"{{{NS}}}{el.tag}"
        res_el.append(obj)
    build_el = ET.SubElement(new_root, f"{{{NS}}}build")
    for item in items:
        if not item.tag.startswith("{"):
            item.tag = f"{{{NS}}}{item.tag}"
        build_el.append(item)

    body = ET.tostring(new_root, encoding="utf-8", xml_declaration=True)

    # Copy the [Content_Types].xml and _rels from the first input; rewrite
    # 3D/3dmodel.model.
    with zipfile.ZipFile(input_paths[0], "r") as ref, zipfile.ZipFile(
        output_path, "w", zipfile.ZIP_DEFLATED
    ) as out:
        for n in ref.namelist():
            if n == "3D/3dmodel.model":
                continue
            out.writestr(n, ref.read(n))
        out.writestr("3D/3dmodel.model", body)

    print(f"Wrote {output_path}: {len(objects)} object(s), {len(items)} build item(s)")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    out = sys.argv[1]
    inputs = []
    names = []
    rest = sys.argv[2:]
    # Collect all .3mf paths first, then any trailing strings as names.
    for arg in rest:
        if arg.lower().endswith(".3mf"):
            inputs.append(arg)
        else:
            names.append(arg)
    merge(out, inputs, names)

// Assembly preview: case + Elecrow device (CrowPanel Advanced 10.1" V1.0).
//
// Renders the chosen front variant ("flush" or "wrap") with the Elecrow
// device model dropped into the cavity for visual fit checking.
//
// Requires elecrow_device.stl in this directory (gitignored, regenerate from
// the upstream STEP file once per machine):
//
//   pip install cadquery
//   python -c "import cadquery as cq; \
//     shp = cq.importers.importStep(r'..\elecrow-ref\3D file\ESP32-P4-10_1-inch-20251230.stp'); \
//     cq.exporters.export(shp, 'elecrow_device.stl', tolerance=0.1, angularTolerance=0.4)"
//
// Render examples:
//   openscad -o flush_iso.png -D 'variant="flush"' --imgsize=1400,900 \
//            --camera=0,0,-8,55,0,30,720 preview_assembly.scad
//   openscad -o wrap_iso.png  -D 'variant="wrap"'  --imgsize=1400,900 \
//            --camera=0,0,-8,55,0,30,720 preview_assembly.scad
//
// Elecrow STEP coords: X=panel-long(±124),
//   Y=depth: glass active surface is at Y=0 (verified: 70 verts at X span 235,
//            Z span 143, matching the panel dimensions exactly).
//            Back of components at Y=-14.5. FFC ribbon cable bits at Y=+2..+3
//            (clipped + ignored — user confirmed not in assembled orientation).
//   Z=panel-short(-90..+205, includes ribbon cable extension)
// My case coords: X=panel-long, Y=panel-short, Z=depth (glass top=0, case bottom=-17.5)
// Transform: rotate(elecrow Y -> my -Z) lands the glass plane exactly at z=0.
// No Z translate. (Earlier renders used translate(+2.9) believing Y=+2.9 was the
// glass — that was the FFC bump, not the glass — putting glass 5.8mm proud.)

variant = "flush";  // "flush" or "wrap"

// Case
color([0.85, 0.85, 0.85, 0.85])
    if (variant == "flush") import("front_flush.stl");
    else                    import("front.stl");

// Device, transformed and clipped to PCB footprint (no ribbon cable)
color([0.30, 0.55, 0.90, 1.0])
    rotate([90, 0, 0])              // elecrow +Y -> my -Z; glass at Y=0 -> z=0
        intersection() {
            import("elecrow_device.stl");
            cube([260, 30, 148], center=true);
        }

// Debug visualization — shifts the measured panel features behind the case
// so both can be inspected together. Not for export; render case.scad alone
// for the printable model.
//
//   & "C:\Program Files\OpenSCAD\openscad.exe" -o debug.png \
//     --imgsize=1400,900 --camera=0,0,0,75,0,25,700 --colorscheme=Tomorrow \
//     debug.scad

include <case.scad>

explode_z = -60;   // shift the panel this far behind the case in -Z

translate([0, 0, explode_z]) {

    // Cover glass (proud above bezel; full frame footprint).
    color("DeepSkyBlue", 0.6)
        translate([frame_cx, frame_cy, -glass_proud / 2])
            cube([frame_W, frame_H, glass_proud], center = true);

    // Display module body (bezel surface to front of driver PCB).
    color("DimGray")
        translate([frame_cx, frame_cy,
                   -(glass_proud + (pcb_thk - glass_proud - pcb_only_thk) / 2)])
            cube([frame_W, frame_H,
                  pcb_thk - glass_proud - pcb_only_thk], center = true);

    // Driver PCB (bare substrate, full PCB outline).
    color("DarkGreen")
        translate([0, 0, -(pcb_thk - pcb_only_thk / 2)])
            cube([outer_W, outer_H, pcb_only_thk], center = true);

    // Active-area outline — thin red plate on the front of the glass.
    color("Red")
        translate([active_cx, active_cy, 0.05])
            cube([active_W, active_H, 0.1], center = true);

    // Mount holes — through the driver PCB.
    color("Yellow")
        for (h = mount_holes)
            translate([h[0], h[1], -pcb_thk + pcb_only_thk / 2])
                cylinder(d = mount_hole_d, h = pcb_only_thk + 0.4, center = true);

    // ESP32-C6 daughter module on the back of the driver PCB.
    color("OrangeRed")
        translate([c6_cx, c6_cy, -(pcb_thk + c6_proud / 2)])
            cube([c6_W, c6_H, c6_proud], center = true);
}

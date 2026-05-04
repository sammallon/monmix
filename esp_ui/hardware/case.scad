// monmix stage-monitor controller — enclosure
//
// Two-piece parametric model for the 3D-printed case wrapping the Elecrow
// CrowPanel Advanced 10.1". Front shell holds the panel via 4 corner posts
// at the PCB mount holes; back cover screws on with M3 self-tap into post
// pilots, nests into the front shell via a perimeter lip, and carries a
// VESA 75 hole pattern with heat-set M4 inserts plus an engraved logo.
//
// Render with (one command per part):
//   & "C:\Program Files\OpenSCAD\openscad.exe" -o front.stl --export-format binstl \
//       -D 'part="front"' case.scad
//   & "C:\Program Files\OpenSCAD\openscad.exe" -o back.stl --export-format binstl \
//       -D 'part="back"'  case.scad
//   & "C:\Program Files\OpenSCAD\openscad.exe" -o back_plain.stl --export-format binstl \
//       -D 'part="back_plain"' case.scad
//   & "C:\Program Files\OpenSCAD\openscad.exe" -o back_logo.stl --export-format binstl \
//       -D 'part="logo"'  case.scad
// back.stl + back_logo.stl are emitted at native coordinates — load both into
// the slicer (back as the main part, back_logo as an additional part) and
// assign each a different filament for a flush two-tone back face.
// back_plain.stl is the back cover with no logo recess — useful for draft
// prints or for placing the logo in the slicer instead.
//
// Coordinate convention:
//   z = 0                 : front of cover glass (= inside of case front face)
//   z = +front_face_t     : outside of case front face (frontmost)
//   z = -stack_thk        : inside of case back wall (= front of back cover)
//   z = -(stack_thk + back_cover_t) : outside of back cover
//   X axis: panel long edge (right is +X when viewing the screen face-up)
//   Y axis: panel short edge (top of the image is +Y)
//   Origin: PCB outer-outline centre, on the cover-glass plane

$fn = 64;

// ---------------------------------------------------------------------------
// PART SELECTOR
// ---------------------------------------------------------------------------
part = "assembly";        // "front" | "back" | "assembly"

// ---------------------------------------------------------------------------
// MEASURED PARAMETERS (2026-05-01).
// ---------------------------------------------------------------------------

outer_W   = 247.0;
outer_H   = 147.0;

frame_W   = 235.0;
frame_H   = 143.0;
frame_off_top    = 2.3;
frame_off_bottom = 1.7;
frame_off_left   = 6.0;
frame_off_right  = 6.0;

active_W  = 222.0;
active_H  = 125.0;
bezel_top    = 15.0;
bezel_bottom = 7.0;
bezel_left   = 13.0;
bezel_right  = 12.0;

glass_proud  = 2.5;

stack_thk    = 17.5;       // glass front → back-cover inside surface; sized to
                           // fit an 8 mm M3 male/female standoff between PCB
                           // back (z=-9.5) and the back cover
pcb_thk      = 9.5;
pcb_only_thk = 1.5;

mount_hole_d  = 3.2;

c6_W   = 16.7;
c6_H   = 12.5;
c6_proud = 2.6;
c6_x_from_left   = 0.0;
c6_y_from_bottom = 21.0;

// SD card slot — top edge. Inner left edge anchored 12 mm from PCB-left edge;
// the right edge defines the width.
sd_left      = -outer_W / 2 + 12.0;
sd_right     = sd_left + 13.0;
sd_top       = -9.0;                  // edge closer to the screen
sd_bot       = -10.5;
sd_x         = (sd_left + sd_right) / 2;
sd_z         = (sd_top + sd_bot) / 2;
sd_W         = sd_right - sd_left;
sd_H         = sd_top - sd_bot;

// Both USB-C connectors share a 9 × 3.4 mm capsule profile (r = 1.7).
usbc_W       = 9.0;
usbc_H       = 3.4;
usbc_corner_r = 1.7;

// First USB-C — exposed cutout on right edge.
usbc_y       = -outer_H / 2 + 63.5;
usbc_z       = -11.2;

// Second USB-C — internal-only recess on right edge (connector sits ~0.5 mm
// proud of the PCB and must clear the case wall but not be exposed).
usbc2_y            = -outer_H / 2 + 84.0;
usbc2_z            = -11.2;
usbc2_recess_depth = 0.7;
usbc2_tolerance    = 1.0;             // per side

// Reset paperclip hole — back wall.
reset_x      = -outer_W / 2 + 5.0;
reset_y      =  outer_H / 2 - 18.0;
reset_d      = 2.0;
connector_slack = 0.3;

// ---------------------------------------------------------------------------
// DESIGN PARAMETERS.
// ---------------------------------------------------------------------------

shell_wall    = 2.4;
front_face_t  = 2.0;
clearance     = 0.4;
corner_r       = 4.0;      // front-shell vertical-edge & front-face fillet
corner_r_back  = 2.5;      // back-cover vertical-edge & back-face fillet
                           // (clamped to back_cover_t/2 — 4 mm would inflate the BB)
front_taper   = 1.5;       // mm of bezel-cutout expansion from glass to outer face
side_taper    = 1.0;       // mm of side-port expansion from inside to outside
screen_overhang = 1.0;     // case lip extends this far inward over the black
                           // glass border, beyond the active-area edge.

// Corner blocks — rectangular, hugging the inner case-wall corner. Each block
// extends from the wall inward and contains a heat-set M3 insert at the PCB
// mount-hole location. Standoff male thread (machine-cut M3) engages this
// insert from below; back-cover screws engage the standoff female end.
//
// NOTE: PCB mount hole sits 2.8 mm from the metal frame edge, so the insert
// wall on the frame side comes out ~0.6 mm thick. Heat-set at low iron
// temperature with gentle pressure to avoid splitting that wall.
post_length        = 8.0;             // block height
m3_insert_d        = 3.6;             // bore for 4 mm OD M3 heat-set insert
m3_insert_h        = 5.0;             // insert length / bore depth
block_x_clearance  = 0.2;             // gap between block edge and frame X edge
block_y_extent     = 8.0;             // how far inward in Y from the case wall

// Back cover.
back_cover_t  = 5.0;
case_screw_clearance_d = 3.5;
csk_top_d     = 6.9;          // M3 flat-head countersink at the outside face
csk_depth     = (csk_top_d - case_screw_clearance_d) / 2;  // 1.7 mm @ 90°
lip_depth     = 1.5;
lip_wall      = 1.5;
lip_clear     = 0.4;

// VESA 75.
vesa_pitch        = 75.0;
vesa_insert_d     = 5.6;     // bore for 6 mm OD M4 inserts
vesa_insert_h     = 6.0;     // insert length (1 mm extends into boss)
vesa_boss_OD      = 9.0;     // solid backstop boss; sized for ~1.7 mm wall
                             // around the larger 5.6 mm bore
vesa_boss_h       = 2.0;     // 1 mm bore extension + 1 mm solid backstop

// Vents — through the back cover (positioned to avoid engraving, VESA
// pattern, mount holes, and reset hole).
back_vent_W      = 8.0;     // along X (slot length)
back_vent_H      = 2.0;     // along Y (slot height)
back_vent_rows_y = [+55, -45];
back_vent_cols_x = [-100, -75, -50, +50, +75, +100];

// Logo + text engraving on the back cover (outside face).
logo_path     = "C:/Users/samallon/OneDrive/church projects/MV Logo/SVG/Monochrome_thick.svg";
logo_W        = 60.0;
logo_aspect   = 772.74 / 383.73;     // from the SVG viewBox
logo_H        = logo_W / logo_aspect;
logo_y        = 14.0;       // shifted up from centre
text_str      = "Mountainview Church";
text_W        = 60.0;       // matched to logo_W
text_line_h   = 6.0;        // approximate post-resize line height for the gap calc
text_gap      = text_line_h / 2;   // user requested: half-line-height below the logo
text_y        = (logo_y - logo_H / 2) - text_gap - text_line_h / 2;
text_font     = "Segoe Script:style=Bold";
engrave_depth = 1.4;       // = 5 × 0.28 mm draft layer / 7 × 0.20 mm fine

// ---------------------------------------------------------------------------
// DERIVED.
// ---------------------------------------------------------------------------

active_cx = (bezel_left   - bezel_right) / 2;
active_cy = (bezel_bottom - bezel_top)   / 2;

frame_cx = (frame_off_left   - frame_off_right) / 2;
frame_cy = (frame_off_bottom - frame_off_top)   / 2;

mount_hole_dx = outer_W / 2 - 1.6 - mount_hole_d / 2;
mount_hole_dy = outer_H / 2 - 1.6 - mount_hole_d / 2;
mount_holes   = [[ mount_hole_dx,  mount_hole_dy],
                 [-mount_hole_dx,  mount_hole_dy],
                 [ mount_hole_dx, -mount_hole_dy],
                 [-mount_hole_dx, -mount_hole_dy]];

c6_cx = -outer_W / 2 + c6_x_from_left + c6_W / 2;
c6_cy = -outer_H / 2 + c6_y_from_bottom + c6_H / 2;

inner_W  = outer_W + 2 * clearance;
inner_H  = outer_H + 2 * clearance;
shell_W  = inner_W + 2 * shell_wall;
shell_H  = inner_H + 2 * shell_wall;
shell_D  = stack_thk + front_face_t;
shell_cz = (front_face_t - stack_thk) / 2;

vesa_off  = vesa_pitch / 2;
vesa_holes = [[ vesa_off,  vesa_off],
              [-vesa_off,  vesa_off],
              [ vesa_off, -vesa_off],
              [-vesa_off, -vesa_off]];

// Post Z extents: from z=0 (post top, against front face inside) to
// z = -post_length (post bottom, where PCB front rests).
post_top_z = 0;
post_bot_z = -post_length;

// ---------------------------------------------------------------------------
// HELPERS
// ---------------------------------------------------------------------------

// Box with rounded vertical edges (radius r) and optional sphere-fillet on
// the +Z face (fillet_top) and/or -Z face (fillet_bot). Mating faces stay
// flat by leaving the corresponding flag false. With a fillet active the
// box's bounding box is exact only when r ≤ d/2.
module rounded_box(w, h, d, r, fillet_top = false, fillet_bot = false) {
    body_zlo = fillet_bot ? -d/2 + r : -d/2;
    body_zhi = fillet_top ?  d/2 - r :  d/2;

    hull() {
        for (x = [-w/2 + r, w/2 - r])
            for (y = [-h/2 + r, h/2 - r])
                translate([x, y, body_zlo])
                    cylinder(r = r, h = body_zhi - body_zlo, $fn = 32);

        if (fillet_top)
            for (x = [-w/2 + r, w/2 - r])
                for (y = [-h/2 + r, h/2 - r])
                    translate([x, y, d/2 - r])
                        sphere(r = r, $fn = 24);

        if (fillet_bot)
            for (x = [-w/2 + r, w/2 - r])
                for (y = [-h/2 + r, h/2 - r])
                    translate([x, y, -d/2 + r])
                        sphere(r = r, $fn = 24);
    }
}

// Tapered rectangular cutout (frustum). Smaller face at z1 (size [w1,h1]),
// larger face at z2 (size [w2,h2]), centred at (cx, cy) in X/Y.
module tapered_rect(cx, cy, w1, h1, z1, w2, h2, z2) {
    hull() {
        translate([cx, cy, z1]) cube([w1, h1, 0.01], center = true);
        translate([cx, cy, z2]) cube([w2, h2, 0.01], center = true);
    }
}

// Capsule (stadium-shape) cross-section in the Y-Z plane: long axis L along
// Y, short axis H along Z, full corner rounding (r = H/2). Extruded x_thick
// along X for use as a thin "slice" inside hull() chains.
module capsule_yz(L, H, x_thick) {
    r = H / 2;
    hull() {
        translate([0, -(L/2 - r), 0])
            rotate([0, 90, 0])
                cylinder(d = H, h = x_thick, center = true);
        translate([0, +(L/2 - r), 0])
            rotate([0, 90, 0])
                cylinder(d = H, h = x_thick, center = true);
    }
}

// ---------------------------------------------------------------------------
// FRONT SHELL
// ---------------------------------------------------------------------------

module shell_solid() {
    translate([0, 0, shell_cz])
        rounded_box(shell_W, shell_H, shell_D, corner_r,
                    fillet_top = true);    // round the visible front face
}

module pcb_cavity() {
    translate([0, 0, -stack_thk / 2])
        cube([inner_W, inner_H, stack_thk + 0.02], center = true);
}

module front_opening_tapered() {
    // Inner face slightly larger than active area so the lip overlaps the
    // black-mask glass border (screen_overhang per side).
    iw = active_W + 2 * screen_overhang;
    ih = active_H + 2 * screen_overhang;
    tapered_rect(
        active_cx, active_cy,
        iw, ih, -0.02,
        iw + 2 * front_taper, ih + 2 * front_taper,
        front_face_t + 0.02);
}

module sd_cutout_tapered() {
    s = connector_slack;
    iw = sd_W + 2 * s;
    ih = sd_H + 2 * s;
    ow = iw + 2 * side_taper;
    oh = ih + 2 * side_taper;
    // Cutout runs along Y; small face at inner Y, large face at outer Y.
    hull() {
        translate([sd_x, inner_H / 2 - 0.02, sd_z])
            cube([iw, 0.01, ih], center = true);
        translate([sd_x, shell_H / 2 + 0.02, sd_z])
            cube([ow, 0.01, oh], center = true);
    }
}

module usbc_cutout_tapered() {
    // Capsule cutout through the right wall, tapering outward.
    s = connector_slack;
    L_in  = usbc_W + 2 * s;
    H_in  = usbc_H + 2 * s;
    L_out = L_in + 2 * side_taper;
    H_out = H_in + 2 * side_taper;
    hull() {
        translate([inner_W / 2 - 0.02, usbc_y, usbc_z])
            capsule_yz(L_in, H_in, 0.01);
        translate([shell_W / 2 + 0.02, usbc_y, usbc_z])
            capsule_yz(L_out, H_out, 0.01);
    }
}

module usbc2_recess() {
    // Internal-only rectangular pocket on the inside of the right wall — gives
    // the second USB-C connector body 0.7 mm of clearance without exposing it.
    // The cube is offset OUTWARD by recess_depth/2 from the cavity surface so
    // it carves into the wall material; the small 0.02 mm thickness overshoot
    // gives a clean Boolean against the cavity.
    L_pocket = usbc_W + 2 * usbc2_tolerance;
    H_pocket = usbc_H + 2 * usbc2_tolerance;
    cz_x     = inner_W / 2 + usbc2_recess_depth / 2;
    translate([cz_x, usbc2_y, usbc2_z])
        cube([usbc2_recess_depth + 0.02, L_pocket, H_pocket], center = true);
}

module corner_blocks() {
    // Rectangular blocks at each corner, hugging the inner case wall. Each
    // layer of the block is contiguous with the perimeter wall — far stronger
    // than detached cylindrical posts. Block X-extent stays clear of the metal
    // display frame; Y-extent reaches inward past the post pilot location.
    block_x_in = inner_W / 2 - frame_W / 2 - block_x_clearance;
    block_y_in = block_y_extent;

    for (sx = [-1, 1])
        for (sy = [-1, 1]) {
            cx = sx * (inner_W / 2 - block_x_in / 2);
            cy = sy * (inner_H / 2 - block_y_in / 2);
            translate([cx, cy, (post_top_z + post_bot_z) / 2])
                cube([block_x_in, block_y_in,
                      abs(post_top_z - post_bot_z)], center = true);
        }
}

module post_inserts() {
    // M3 heat-set insert bores in each corner block. Installed from the
    // bottom face of the block (z = post_bot_z); insert seats with knurls
    // gripped into the bore wall, threaded body extending up into the block.
    for (h = mount_holes)
        translate([h[0], h[1], post_bot_z - 0.01])
            cylinder(d = m3_insert_d,
                     h = m3_insert_h + 0.02);
}

module shell_body() {
    difference() {
        shell_solid();
        pcb_cavity();
        front_opening_tapered();
        sd_cutout_tapered();
        usbc_cutout_tapered();
        usbc2_recess();
    }
}

module front_shell() {
    difference() {
        union() {
            shell_body();
            corner_blocks();
        }
        post_inserts();
    }
}

// ---------------------------------------------------------------------------
// BACK COVER
// ---------------------------------------------------------------------------

module back_vents() {
    // Through-cuts in the back cover for ventilation. Positioned to clear
    // the engraving region, the VESA pattern, and the corner mount holes.
    cz = -(stack_thk + back_cover_t / 2);
    for (y = back_vent_rows_y)
        for (x = back_vent_cols_x)
            translate([x, y, cz])
                cube([back_vent_W, back_vent_H,
                      back_cover_t + 0.4], center = true);
}

module vesa_backstop_bosses() {
    // Solid bosses on the inside of the cover at each VESA position.
    // Acts as a hard depth-stop for the heat-set inserts (the 5.1 mm bore
    // in the cover above terminates against the boss top at z = -17).
    boss_top_z = -stack_thk + vesa_boss_h;
    boss_cz    = (-stack_thk + boss_top_z) / 2;

    for (v = vesa_holes)
        translate([v[0], v[1], boss_cz])
            cylinder(d = vesa_boss_OD, h = vesa_boss_h, center = true);
}

module back_cover_lip() {
    // Hollow ring extending UP into the front shell's cavity from the back
    // cover's top face. Sized with print-fit clearance.
    lip_outer_W = inner_W - 2 * lip_clear;
    lip_outer_H = inner_H - 2 * lip_clear;
    lip_inner_W = lip_outer_W - 2 * lip_wall;
    lip_inner_H = lip_outer_H - 2 * lip_wall;
    cz = -stack_thk + lip_depth / 2;

    difference() {
        translate([0, 0, cz])
            cube([lip_outer_W, lip_outer_H, lip_depth], center = true);
        translate([0, 0, cz])
            cube([lip_inner_W, lip_inner_H, lip_depth + 0.02], center = true);
    }
}

module engraving_2d() {
    // 2D shape of the logo + text engraving. Used both as a subtractor (to
    // create the recess in the back cover) and as a positive solid (the
    // "logo" insert printed in a second material).
    //
    // Mirrored along X because the engraving lives on the case's BACK face;
    // when the user looks at the back of the device, the X axis is flipped
    // relative to the face-up SCAD coords, so the unmirrored shape would
    // read right-to-left.
    mirror([1, 0, 0]) {
        translate([-logo_W / 2, logo_y - logo_H / 2])
            resize([logo_W, 0, 0], auto = true)
                import(logo_path);
        translate([0, text_y])
            resize([text_W, 0, 0], auto = true)
                text(text_str,
                     size  = 10,
                     font  = text_font,
                     halign= "center",
                     valign= "center");
    }
}

module back_cover_engraving() {
    // Subtractor for the recess. Buffered slightly past the cover surfaces
    // for clean Boolean.
    z_outside = -(stack_thk + back_cover_t);
    translate([0, 0, z_outside - 0.01])
        linear_extrude(height = engrave_depth + 0.02)
            engraving_2d();
}

module back_cover_logo_solid() {
    // Positive solid that sits flush in the recess. Print this in a
    // contrasting filament; load it as a separate part in the slicer
    // alongside back.stl — both files are emitted at native coordinates
    // so they line up automatically.
    z_outside = -(stack_thk + back_cover_t);
    translate([0, 0, z_outside])
        linear_extrude(height = engrave_depth)
            engraving_2d();
}

module back_cover_plate() {
    cz = -(stack_thk + back_cover_t / 2);
    translate([0, 0, cz])
        rounded_box(shell_W, shell_H, back_cover_t, corner_r_back,
                    fillet_bot = true);    // round the visible back face
}

module back_cover(engrave = true) {
    difference() {
        union() {
            back_cover_plate();
            back_cover_lip();
            vesa_backstop_bosses();
        }

        // Case-joining clearance holes (at corner-post pilots) with M3
        // flat-head countersink at the outside face.
        for (h = mount_holes) {
            translate([h[0], h[1], -(stack_thk + back_cover_t / 2)])
                cylinder(d = case_screw_clearance_d,
                         h = back_cover_t + lip_depth + 0.04, center = true);
            translate([h[0], h[1], -(stack_thk + back_cover_t) - 0.01])
                cylinder(d1 = csk_top_d, d2 = case_screw_clearance_d,
                         h = csk_depth + 0.02);
        }

        // VESA 75 heat-set insert bores. Extend 1 mm into the backstop boss
        // for the full 6 mm insert depth.
        for (v = vesa_holes)
            translate([v[0], v[1], -(stack_thk + back_cover_t) - 0.01])
                cylinder(d = vesa_insert_d,
                         h = vesa_insert_h + 0.02);

        // Reset paperclip hole.
        translate([reset_x, reset_y, -(stack_thk + back_cover_t / 2)])
            cylinder(d = reset_d + 2 * connector_slack,
                     h = back_cover_t + 0.04, center = true);

        back_vents();

        if (engrave) back_cover_engraving();
    }
}

// ---------------------------------------------------------------------------
// TOP-LEVEL DISPATCH
// ---------------------------------------------------------------------------

if      (part == "front")      front_shell();
else if (part == "back")       back_cover();
else if (part == "back_plain") back_cover(engrave = false);
else if (part == "logo")       back_cover_logo_solid();
else {                                  // "assembly"
    front_shell();
    back_cover();
    color("Goldenrod") back_cover_logo_solid();
}

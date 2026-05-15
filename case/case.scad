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
//   & "C:\Program Files\OpenSCAD\openscad.exe" -o front_snap.stl --export-format binstl \
//       -D 'part="front_snap"' case.scad
//   & "C:\Program Files\OpenSCAD\openscad.exe" -o back.stl --export-format binstl \
//       -D 'part="back"'  case.scad
//   & "C:\Program Files\OpenSCAD\openscad.exe" -o back_plain.stl --export-format binstl \
//       -D 'part="back_plain"' case.scad
//   & "C:\Program Files\OpenSCAD\openscad.exe" -o back_logo.stl --export-format binstl \
//       -D 'part="logo"'  case.scad
// The flush variant is ARCHIVED — see comment block above the FLUSH-BEZEL
// section. To render it for revival work:
//   & "C:\Program Files\OpenSCAD\openscad.exe" -o front_flush.stl --export-format binstl \
//       -D 'part="front_flush"' case.scad
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
part = "assembly";        // "front" | "front_snap" | "back" | "assembly"
                          //   front_flush is ARCHIVED — render explicitly
                          //   via -D 'part="front_flush"' to revive.

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
screen_overhang = 1.5;     // case lip extends this far inward over the black
                           // glass border, beyond the active-area edge.
                           // Bumped from 1.0 to 1.5 (2026-05-14) for
                           // production PETG print: +0.5 mm on each side
                           // gives +1 mm horizontal and +1 mm vertical
                           // total opening clearance around the active area.

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
// PRINT-FIT CORRECTIONS (from 0.2 mm draft 2026-05-04, new filament roll).
// Orientation: display up, USB cluster on right — +X right, +Y top, -Z back.
// Tweak cutout cuts to match the printed result; the physical measurements
// above are unchanged.
// ---------------------------------------------------------------------------

// Active-display cutout: shift up; right-edge-only extension.
front_opening_dy           = 0.5;
front_opening_extend_right = 0.5;

// SD-card cutout: shift toward back; back-edge-only extension.
sd_cutout_shift_back  = 0.4;
sd_cutout_extend_back = 0.4;

// Flush-variant glass pocket clearance — per side. The pocket is built
// from the glass dimensions plus a clearance on each side; making one
// side smaller fills the gap on that side without moving the pocket
// elsewhere.
//
// Observed 2026-05-13: 1.1 mm gap at top of screen and 1.2 mm gap at
// right, with USB cluster oriented to +X. Bottom and left gaps not
// noted (treated as ~0). Setting nominal top/right clearance to 0 mm
// brings the pocket wall up against the cover-glass edge in the model;
// print-fit expansion (~0.05-0.2 mm per side empirically) leaves a
// hairline visible gap rather than the gross 1.1/1.2 mm gap. Bottom
// and left clearances stay at 0.5 mm so the LCD frame still slides up
// through the pocket easily on assembly.
//
// Risk: a future print with significantly less expansion than this one
// would have the frame binding on top/right edges; mitigation is to
// either bump these clearances up slightly (0.1-0.2 mm) or sand 0.1 mm
// off the pocket wall on the binding side.
flush_glass_clear_top   = 0.0;
flush_glass_clear_bot   = 0.5;
flush_glass_clear_left  = 0.5;
flush_glass_clear_right = 0.0;

// ---------------------------------------------------------------------------
// FLUSH-BEZEL VARIANT (alternate front shell).
//
// **ARCHIVED 2026-05-14** — preserved in source but no longer dispatched.
//
// Why: the printed prototype (2026-05-13) surfaced three problems that
// individually are small but together kill the variant for stage use:
//
//   1. The LCD ribbon cable at the top edge of the screen interferes with
//      the inner case wall: it either shows in the gap between the case
//      and the cover glass, or pushes that wall outward as the assembly
//      is closed. The wrap-around `front` variant doesn't share this
//      because its inner lip overhangs that region and hides the cable.
//
//   2. `flush_stiffener_ribs` (added 2026-05-13) reduced the long-wall
//      flex visibly but not enough. A real fix would need to thicken the
//      top/bottom walls (a major outer-geometry change), which isn't
//      justified for a cosmetic variant.
//
//   3. The cover glass shows slight separation from its own printed black
//      mask at the four corners. Cosmetic only, but the variant's whole
//      selling point was visual sleekness — and the corners undermined
//      that on the prototype.
//
// What stays committed: the full source for this variant (parameters,
// modules, modeled clearances) plus the final-iteration STL/3MF in
// case/front_flush.{stl,3mf}, so the variant can be revived without
// re-deriving the geometry. Removed: the `"front_flush"` dispatch entry,
// so no default export targets this. Use `-D 'part="front_flush"'` on
// the openscad command line if you want to render it from source.
//
// ---------------------------------------------------------------------------
// FLUSH-BEZEL VARIANT — original design rationale (preserved verbatim)
//
// Sits flush with the cover-glass front instead of wrapping over it. The
// existing `front` part is unchanged; this is an additional `front_flush`
// option to print and evaluate side-by-side.
//
// Geometry: the case wall steps inward at the glass plane to a tight
// perimeter pocket sized to the cover glass + assembly clearance. Below
// the step, the cavity is the full PCB-clearance footprint (so the PCB
// has room); above the step, the wall is flush against the glass edge
// (so there's no visible gap around the glass). The LCD assembly slides
// up through the pocket from below during back-cover-off assembly — same
// procedure as the wrap-around.
//
// Trade-offs vs `front`:
//   + Cleaner look; glass is at the same plane as the case top edge.
//   + Simpler print: open-mouth-down, no front-face-down bridging.
//   - Less drop protection; no plastic above glass top.
//   - Tighter assembly tolerance: the LCD frame must fit through the
//     pocket (0.5 mm per-side clearance default).
//   - Wall is much thicker around the glass perimeter (8.5 mm long-edge,
//     4.5 mm short-edge) to fill what the wrap-around lip otherwise
//     covered. This is unavoidable — same plastic mass, just inverted.
// ---------------------------------------------------------------------------

// Round-over of the new exposed top edge of the sidewall (the edge at z = 0
// where the front face used to start). Small radius keeps it finger-friendly
// without violating "flush".
flush_top_fillet               = 1.0;

// Cover-glass dimensions (measured against the actual CrowPanel Advanced
// 10.1" V1.0 panel: glass perimeter equals metal-frame perimeter — 6 mm in
// from each long PCB edge, 2 mm in from each short PCB edge). Adjust if
// fitting a different display revision.
flush_glass_W                  = 235.0;
flush_glass_H                  = 143.0;

// Glass-pocket clearance is asymmetric — per-side values declared in the
// PRINT-FIT CORRECTIONS section above so they group with the other
// empirical adjustments. The pocket is built by adding each side's
// clearance to the cover-glass extent on that side, with the pocket
// centred on the resulting (asymmetric) midpoint. Loose enough that the
// LCD frame slides up through the pocket without scraping; tight enough
// that the visible gap around the glass reads as a hairline.

// Glass pocket Z extent. The pocket sits between the metal-frame top
// plane and the glass top plane; nothing else lives in this Z range
// except the cover glass itself.
flush_surround_bot_z           = -glass_proud;   // = -2.5 = frame top
flush_surround_top_z           =  0;             // = glass top

// Corner-block top truncation for the flush variant. The shared
// corner_blocks() module raises blocks to z=0 to support the wrap-around
// front face. With no front face, the top of the block would project
// into the glass-pocket Z range; raise the block only to the glass-
// surround bottom plane so the block + surround form a continuous solid
// from the insert face (z = -8) up to the case front (z = 0) at each
// corner.
//
// post_bot_z = -post_length = -8; insert occupies z=-8..-3 (m3_insert_h=5).
// Setting top to flush_surround_bot_z (-2.5) puts 0.5 mm of plastic over
// the insert top (was previously -3.5, which left the bore opening into
// open air above the truncated block — a 1 mm gap above the block before
// the glass_surround took over).
flush_block_top_z              = flush_surround_bot_z;

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
    iw = active_W + front_opening_extend_right + 2 * screen_overhang;
    ih = active_H + 2 * screen_overhang;
    // Right-edge-only extension shifts cx by half so the left edge stays put.
    cx = active_cx + front_opening_extend_right / 2;
    cy = active_cy + front_opening_dy;
    tapered_rect(
        cx, cy,
        iw, ih, -0.02,
        iw + 2 * front_taper, ih + 2 * front_taper,
        front_face_t + 0.02);
}

module sd_cutout_tapered() {
    s = connector_slack;
    iw = sd_W + 2 * s;
    ih = sd_H + sd_cutout_extend_back + 2 * s;
    ow = iw + 2 * side_taper;
    oh = ih + 2 * side_taper;
    // Back-edge-only extension shifts cz by half so the screen-side edge
    // stays put; then translate the whole cutout toward the back.
    cz = sd_z - sd_cutout_shift_back - sd_cutout_extend_back / 2;
    // Cutout runs along Y; small face at inner Y, large face at outer Y.
    hull() {
        translate([sd_x, inner_H / 2 - 0.02, cz])
            cube([iw, 0.01, ih], center = true);
        translate([sd_x, shell_H / 2 + 0.02, cz])
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
// FRONT SHELL — FLUSH VARIANT
// ---------------------------------------------------------------------------

module shell_solid_flush() {
    // Outer envelope of the flush variant: same X/Y footprint as the
    // wrap-around shell, but truncated at z = 0 (no front_face_t cap).
    // Sphere-fillets are placed at the new top edge so the exposed
    // sidewall lip is rounded over rather than left as a sharp corner.
    flush_d  = stack_thk;
    flush_cz = -stack_thk / 2;
    translate([0, 0, flush_cz])
        rounded_box(shell_W, shell_H, flush_d, corner_r,
                    fillet_top = true);
}

module glass_surround() {
    // Perimeter ring that closes the gap between the cover-glass edge and
    // the case sidewall. Without it the flush variant would expose a
    // 6.4 mm trough on long edges and a 2.4 mm trough on short edges
    // (cavity is sized for the PCB; glass is smaller than the PCB).
    //
    // Sits in the Z range between the metal-frame top plane and the glass
    // top plane (z = -glass_proud .. 0). The inner pocket is sized
    // asymmetrically via the per-side `flush_glass_clear_*` parameters
    // (top/bottom/left/right), then centred on the resulting midpoint so
    // each pocket wall is exactly `clear_<side>` from the matching glass
    // edge. The LCD frame slides up through the pocket during back-
    // cover-off assembly, so each clearance must be ≥ 0 nominally (with
    // print expansion providing the actual sliding clearance).
    //
    // Doubles as the forward-push stop for the panel: the frame top
    // bears against the ring's lower face if the assembly is pushed
    // toward the screen.
    pocket_W  = flush_glass_W + flush_glass_clear_left + flush_glass_clear_right;
    pocket_H  = flush_glass_H + flush_glass_clear_top  + flush_glass_clear_bot;
    pocket_cx = frame_cx + (flush_glass_clear_right - flush_glass_clear_left) / 2;
    pocket_cy = frame_cy + (flush_glass_clear_top   - flush_glass_clear_bot)  / 2;
    h         = abs(flush_surround_top_z - flush_surround_bot_z);
    cz        = (flush_surround_top_z + flush_surround_bot_z) / 2;

    difference() {
        translate([0, 0, cz])
            cube([inner_W, inner_H, h], center = true);
        translate([pocket_cx, pocket_cy, cz])
            cube([pocket_W, pocket_H, h + 0.02], center = true);
    }
}

module shell_body_flush() {
    // Like shell_body() but no front-face cutouts (the whole front is open).
    // Side cutouts (SD, USB-C, USB-C2 recess) survive verbatim.
    difference() {
        shell_solid_flush();
        pcb_cavity();
        sd_cutout_tapered();
        usbc_cutout_tapered();
        usbc2_recess();
    }
}

module corner_blocks_flush() {
    // Same plan-view footprint as corner_blocks(), but truncated in Z so
    // the block tops sit at flush_block_top_z (well below the cover-glass
    // plane). Used only by the flush variant — the wrap-around design
    // still relies on corner_blocks() rising to z=0.
    block_x_in = inner_W / 2 - frame_W / 2 - block_x_clearance;
    block_y_in = block_y_extent;

    block_h = abs(flush_block_top_z - post_bot_z);
    cz      = (flush_block_top_z + post_bot_z) / 2;

    for (sx = [-1, 1])
        for (sy = [-1, 1]) {
            cx = sx * (inner_W / 2 - block_x_in / 2);
            cy = sy * (inner_H / 2 - block_y_in / 2);
            translate([cx, cy, cz])
                cube([block_x_in, block_y_in, block_h], center = true);
        }
}

module flush_stiffener_ribs() {
    // Vertical buttresses along the inside of each sidewall, sitting in
    // the dead space between the metal display-frame edge and the inner
    // case wall, in the Z range above the PCB front (z = -8) and below
    // the glass-surround bottom (z = -2.5). Stiffens the long sidewalls
    // (top/bottom of screen) which were observed to flex outward away
    // from the device on a printed sample; the short sidewalls flex
    // less but get a couple of ribs too.
    //
    // Ribs are vertical extrusions in the print orientation (mouth-down),
    // so they print as local thickenings of the inner wall with no
    // overhangs or support.
    //
    // Component-clearance audit (2026-05-13):
    //  - ESP32-C6 daughter module sits on the BACK of the PCB
    //    (z = [-12.1, -9.5]) and never enters the rib Z range.
    //  - SD-card cutout is on the top wall but at z = [-10.5, -9.0],
    //    also below the rib Z range.
    //  - USB-C cutouts cross the right wall at z = [-13.9, -8.5];
    //    short-edge rib at z = [-7.5, -2.5] sits above them.
    //  - USB-C2 internal recess at z = [-13.9, -8.5] likewise below.
    //  - Front-side PCB components other than the LCD assembly are
    //    not modelled here; the rib X/Y positions are chosen to clear
    //    documented features but assume nothing else protrudes into
    //    the perimeter dead space above the PCB.
    rib_thk      = 2.5;
    rib_z_top    = flush_surround_bot_z - 0.01;
    rib_z_bot    = post_bot_z + 0.5;
    rib_h        = abs(rib_z_top - rib_z_bot);
    rib_cz       = (rib_z_top + rib_z_bot) / 2;

    inset_top    = inner_H / 2 - (outer_H / 2 - frame_off_top);
    inset_bot    = inner_H / 2 - (outer_H / 2 - frame_off_bottom);
    inset_left   = inner_W / 2 - (outer_W / 2 - frame_off_left);
    inset_right  = inner_W / 2 - (outer_W / 2 - frame_off_right);

    // Long-edge rib X positions. Spaced between corner blocks
    // (|x| < ~117) and clear of the SD-card cutout at x ≈ -105 on the
    // top wall (SD cutout is Z-disjoint from the ribs but keeping the
    // X spacing wide is cheap insurance).
    long_rib_xs  = [-70, -25, +25, +70];

    // Short-edge rib Y positions. Clear of both USB-C cutouts on the
    // right wall: USB1 centred at y = -10 (range [-14.5, -5.5]) and
    // USB2 centred at y = +10.5 (range [+6, +15]).
    short_rib_ys = [-40, +40];

    for (rx = long_rib_xs) {
        translate([rx, inner_H / 2 - inset_top / 2, rib_cz])
            cube([rib_thk, inset_top, rib_h], center = true);
        translate([rx, -inner_H / 2 + inset_bot / 2, rib_cz])
            cube([rib_thk, inset_bot, rib_h], center = true);
    }
    for (ry = short_rib_ys) {
        translate([-inner_W / 2 + inset_left / 2, ry, rib_cz])
            cube([inset_left, rib_thk, rib_h], center = true);
        translate([+inner_W / 2 - inset_right / 2, ry, rib_cz])
            cube([inset_right, rib_thk, rib_h], center = true);
    }
}

module front_shell_flush() {
    difference() {
        union() {
            shell_body_flush();
            corner_blocks_flush();
            glass_surround();
            flush_stiffener_ribs();
        }
        post_inserts();
    }
}

// ---------------------------------------------------------------------------
// FRONT SHELL — SNAP-FIT VARIANT
//
// Same outer shell as `front` (wrap-around) but with retention ribs that
// hold the PCB front-to-back via a snap-fit:
//
//   * Front ribs are small ledges along the inner walls at z = post_bot_z
//     (PCB front face). The PCB rests against their lower face.
//
//   * Back ribs sit behind the PCB. They taper out of the wall in the
//     direction of insertion (PCB pushed up from the back-cover opening
//     toward the screen), so the case wall flexes outward as the PCB
//     slides past, then snaps back to lock the PCB in place.
//
// With the ribs holding the PCB, the M3 standoffs that previously had to
// be aligned at the corner mount holes are no longer required for PCB
// retention. The corner blocks + heat-set inserts remain unchanged in
// this variant — standoffs still serve as back-cover screw anchors
// (their PCB-clamping role just becomes optional).
//
// Print orientation: face-down, same as the wrap-around variant. The
// PCB-facing chamfer on the back rib is sized for ~45 deg slope so the
// rib's retention surface prints without support. The insertion ramp
// uses a gentler ~27 deg slope so the PCB slides up easily during
// assembly.
//
// Back-rib XY positions were audited against the Elecrow STEP file on
// 2026-05-14 (audit script in .copilot/) — all 12 rib positions clear
// of back-side PCB components.
// ---------------------------------------------------------------------------

// Front-side retention rib: rectangular ledge against PCB front.
snap_front_rib_thk    = 1.5;   // Z extent (-8 down to -6.5)
snap_front_rib_depth  = 2.0;   // protrusion into cavity from inner wall.
                                // Long-wall constraint: screen-frame inner
                                // edge at Y = +/-71.5; inner case wall at
                                // Y = +/-73.9; depth 2.0 keeps rib tip at
                                // Y = +/-71.9, clear of the frame by 0.4
                                // mm. Short walls have more room (6.4 mm
                                // dead space) but use the same value for
                                // uniformity.
snap_front_rib_width  = 8.0;   // length along the wall direction

// Back-side retention rib: tapered barb that snaps past the PCB on
// insertion and retains it afterwards. Cross-section in (depth, Z):
//
//   wall ----- + z_ret = -9.8
//              | \           <- chamfer (~45 deg, FDM-printable overhang)
//              |  \
//              |   + z_chamfer = -11.3, depth = 1.5
//              |   |         <- flat tip (small Z section for tolerance)
//              |   + z_flat = -11.6, depth = 1.5
//              |  /
//              | /           <- insertion ramp (~27 deg, gentle slope)
//              |/
//   wall ----- + z_bot = -14.5
//
snap_back_rib_z_ret    = -9.8;  // retention face Z at wall (PCB back at
                                 //   -9.5, so 0.3 mm Z tolerance pocket)
snap_back_rib_chamfer  = 1.5;   // chamfer Z extent (45 deg at full depth)
snap_back_rib_flat     = 0.3;   // small flat at the barb tip
snap_back_rib_ramp     = 2.9;   // insertion ramp Z extent (~27 deg)
snap_back_rib_depth    = 1.5;   // max protrusion into cavity
snap_back_rib_width    = 6.0;   // length along the wall direction
// Total rib Z extent: -9.8 .. -14.5.

// Rib XY positions. Matches the audited-clear flush_stiffener_ribs
// layout; front and back ribs share positions on this first cut.
snap_long_rib_xs       = [-70, -25, +25, +70];   // top + bottom walls
snap_short_rib_ys      = [-40, +40];             // left + right walls

module front_rib_block() {
    // Local frame: wall at Y=0, rib extends in +Y (into cavity).
    // Width along X. Z anchored so rib's BOTTOM face is at post_bot_z
    // (= -8 = PCB front face) — PCB rests on this lower face.
    rib_thk   = snap_front_rib_thk;
    rib_depth = snap_front_rib_depth;
    rib_w     = snap_front_rib_width;
    translate([0, rib_depth / 2, post_bot_z + rib_thk / 2])
        cube([rib_w, rib_depth, rib_thk], center = true);
}

module back_rib_block() {
    // Local frame: wall at Y=0, rib extends in +Y (into cavity). Width
    // along X. Cross-section is a 2D polygon (depth, Z) that's extruded
    // along the rib width, then rotated so the extrusion direction
    // becomes case-X.
    rib_w     = snap_back_rib_width;
    rib_depth = snap_back_rib_depth;
    z_ret     = snap_back_rib_z_ret;
    z_chamfer = z_ret     - snap_back_rib_chamfer;
    z_flat    = z_chamfer - snap_back_rib_flat;
    z_bot     = z_flat    - snap_back_rib_ramp;

    // Slight -0.01 mm overlap into wall on the two wall-side vertices so
    // the rib unions cleanly with the shell.
    rotate([90, 0, 90])
        linear_extrude(height = rib_w, center = true)
            polygon([
                [-0.01,    z_ret],     // wall, retention face top
                [rib_depth, z_chamfer], // chamfer end, max depth
                [rib_depth, z_flat],    // flat-tip end
                [-0.01,    z_bot]      // wall, ramp end
            ]);
}

// Per-wall rib placement. The rib block modules above are authored as if
// the wall is at Y=0 with the rib protruding in +Y. For each of the four
// inner walls we translate to the wall position and rotate so the rib
// protrudes inward into the cavity.
//
//   Top wall    (Y = +inner_H/2):  rotate Z by 180  (+Y of block -> -Y of case)
//   Bottom wall (Y = -inner_H/2):  identity         (+Y of block -> +Y of case)
//   Left wall   (X = -inner_W/2):  rotate Z by  +90 (+Y of block -> +X of case)
//   Right wall  (X = +inner_W/2):  rotate Z by  -90 (+Y of block -> -X of case)

module front_retention_ribs() {
    for (rx = snap_long_rib_xs) {
        translate([rx, +inner_H / 2, 0]) rotate([0, 0, 180]) front_rib_block();
        translate([rx, -inner_H / 2, 0])                    front_rib_block();
    }
    for (ry = snap_short_rib_ys) {
        translate([-inner_W / 2, ry, 0]) rotate([0, 0,  90]) front_rib_block();
        translate([+inner_W / 2, ry, 0]) rotate([0, 0, -90]) front_rib_block();
    }
}

module back_retention_ribs() {
    for (rx = snap_long_rib_xs) {
        translate([rx, +inner_H / 2, 0]) rotate([0, 0, 180]) back_rib_block();
        translate([rx, -inner_H / 2, 0])                    back_rib_block();
    }
    for (ry = snap_short_rib_ys) {
        translate([-inner_W / 2, ry, 0]) rotate([0, 0,  90]) back_rib_block();
        translate([+inner_W / 2, ry, 0]) rotate([0, 0, -90]) back_rib_block();
    }
}

module front_shell_snap() {
    // Wrap-around front shell + retention ribs. Corner blocks and post
    // inserts (and therefore the standoff + back-cover-screw chain) are
    // inherited from front_shell() unchanged.
    union() {
        front_shell();
        front_retention_ribs();
        back_retention_ribs();
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

if      (part == "front")       front_shell();
else if (part == "front_snap")  front_shell_snap();
else if (part == "front_flush") front_shell_flush();   // archived — see header
else if (part == "back")        back_cover();
else if (part == "back_plain")  back_cover(engrave = false);
else if (part == "logo")        back_cover_logo_solid();
else {                                  // "assembly"
    front_shell();
    back_cover();
    color("Goldenrod") back_cover_logo_solid();
}

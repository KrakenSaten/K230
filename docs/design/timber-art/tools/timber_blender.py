"""PocketTimber art pipeline: the master scene, the renders and the exports.

Runs inside Blender (5.2 LTS; EEVEE), either from a live session

    exec(open("C:/K230/docs/design/timber-art/tools/timber_blender.py").read())

or in batch

    blender -b -P docs/design/timber-art/tools/timber_blender.py -- proof

It builds its own scene ("TimberArt") and never touches, saves or renders
any other scene in the file, so it is safe to run in a session that holds
other work. The scene is written to assets-src/timber-art.blend with
bpy.data.libraries.write(), which exports that scene alone.

Everything below is fixed by the canonical projection in
docs/apps/POCKETTIMBER_ART.md: a 2:1 dimetric orthographic camera at
azimuth 45 degrees and elevation 30 degrees, one block width = 36 px on
screen along a diagonal (TIMBER_VIEW_SCALE), one layer = 26 px
(TIMBER_VIEW_LAYER_PX). The engine draws +x toward the lower right, which
is the mirror image of a right-handed camera at the tower's near corner,
so every sprite is mirrored horizontally on export and the key light is
placed so that it reads from the upper left after the flip.

Stages (argument, or the global TIMBER_STAGE before exec):
    scene    build the scene only
    proof    scene + the proof set: block x, block y, felt, shadow, study,
             composition, anchors, blend file
    all      proof + every production sprite (tones x poses)
"""
import json
import math
import os
import struct
import sys
import zlib

import bpy
import numpy as np
from mathutils import Vector


# ---- PNG on the standard library, shared in spirit with tools/pngio.py --------

def _chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)


def write_png(path, arr):
    """arr: top-down (h, w, 4) uint8."""
    h, w = arr.shape[:2]
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(arr[y].tobytes())
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) +
                _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b""))


def read_png(path):
    """(w, h, rows) with rows a flat list of (r, g, b, a) per pixel, top-down;
    8-bit RGB or RGBA, non-interlaced, which is what Blender writes."""
    with open(path, "rb") as f:
        data = f.read()
    pos = 8
    idat = bytearray()
    w = h = color_type = 0
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            w, h, _, color_type, _, _, _ = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.extend(body)
    channels = 4 if color_type == 6 else 3
    stride = w * channels
    raw = zlib.decompress(bytes(idat))
    out = np.zeros((h, w, 4), dtype=np.uint8)
    prev = np.zeros(stride, dtype=np.int32)
    p = 0
    for y in range(h):
        filt = raw[p]
        line = np.frombuffer(raw, dtype=np.uint8, count=stride, offset=p + 1).astype(np.int32)
        p += 1 + stride
        if filt == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filt == 2:
            line = (line + prev) & 0xFF
        elif filt == 3:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif filt == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        prev = line
        px = line.reshape(w, channels)
        out[y, :, :channels] = px
        if channels == 3:
            out[y, :, 3] = 255
    return w, h, out.reshape(h * w, 4).tolist()

# ---- canonical numbers ------------------------------------------------------

ROOT = "C:/K230/docs/design/timber-art"
RENDERED = ROOT + "/rendered"
STUDY = ROOT + "/study"
SRC = ROOT + "/assets-src"

# Block: the classic proportions, 3 : 1 : 0.6, in metres.
BLOCK_L = 0.075
BLOCK_W = 0.025
BLOCK_H = 0.015
BEVEL = 0.0008           # 0.8 mm, a worn edge, not a chamfer

SCALE_PX = 36            # TIMBER_VIEW_SCALE: px per width along a diagonal
LAYER_PX = 26            # TIMBER_VIEW_LAYER_PX, 0.6 * sqrt(2) * cos(30) * 36 = 26.46
AZIMUTH = 45.0
ELEVATION = 30.0
# One width along x projects to a horizontal run of cos(45) widths.
PX_PER_M = SCALE_PX / (math.cos(math.radians(AZIMUTH)) * BLOCK_W)

BLOCK_RES = (208, 160)   # a block with a tilt fits with margin
STUDY_RES = (420, 520)
COMP_RES = (528, 640)
PAD = 2                  # transparent pixels kept round a cropped sprite

# Wood tones (sRGB). Base is a light-to-medium warm hardwood; the variants
# are a step lighter and a step darker, not another species.
TONES = {
    0: ((201, 166, 112), (166, 128, 78)),   # base: face colour, grain colour
    1: ((214, 182, 130), (178, 143, 94)),   # lighter
    2: ((186, 149, 96), (150, 113, 66)),    # darker
}
FELT_RGB = (27, 58, 42)   # #1B3A2A, dark muted green
POSES = {0: 0.0, 1: 14.0, 2: -22.0}   # tilt about the block's long axis, degrees

SAMPLES = 64


def srgb_to_linear(c):
    out = []
    for v in c:
        v = v / 255.0
        out.append(v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4)
    return tuple(out)


def ensure_dirs():
    for d in (RENDERED, STUDY, SRC):
        os.makedirs(d, exist_ok=True)


# ---- the scene --------------------------------------------------------------

def get_scene():
    """The TimberArt scene, built if it is not there, untouched otherwise."""
    scene = bpy.data.scenes.get("TimberArt")
    if scene:
        return scene
    scene = bpy.data.scenes.new("TimberArt")
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.film_transparent = True
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "8"
    scene.render.resolution_percentage = 100
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.display_settings.display_device = "sRGB"
    if hasattr(scene, "eevee"):
        scene.eevee.taa_render_samples = SAMPLES
    scene.unit_settings.system = "METRIC"

    # World: a cool, neutral, dim fill. It is the only fill light.
    world = bpy.data.worlds.new("TimberWorld")
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    bg.inputs["Color"].default_value = (0.16, 0.175, 0.20, 1.0)
    bg.inputs["Strength"].default_value = 1.0
    scene.world = world

    # Camera: orthographic, azimuth 45, elevation 30, at the near corner.
    cam_data = bpy.data.cameras.new("TimberCamera")
    cam_data.type = "ORTHO"
    cam = bpy.data.objects.new("TimberCamera", cam_data)
    scene.collection.objects.link(cam)
    cam.rotation_euler = (math.radians(90.0 - ELEVATION), 0.0, math.radians(180.0 - AZIMUTH))
    cam.location = Vector((0.6, 0.6, 0.6 * math.tan(math.radians(ELEVATION)) * math.sqrt(2)))
    scene.camera = cam

    # Key: one warm sun, soft, from what will be the upper left once the
    # sprite is mirrored: toward +y more than +x, mostly from above.
    key_data = bpy.data.lights.new("TimberKey", "SUN")
    key_data.energy = 3.2
    key_data.color = (1.0, 0.95, 0.88)
    key_data.angle = math.radians(6.0)
    key = bpy.data.objects.new("TimberKey", key_data)
    scene.collection.objects.link(key)
    toward_light = Vector((0.35, 0.75, 1.2)).normalized()
    key.rotation_euler = (-toward_light).to_track_quat("-Z", "Y").to_euler()
    key.location = toward_light * 1.0

    # Ground: a matte plane far larger than any tower, for the study and the
    # composition; hidden for sprite renders.
    ground = bpy.data.objects.new("TimberGround", bpy.data.meshes.new("TimberGround"))
    ground.data.from_pydata([(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)], [], [(0, 1, 2, 3)])
    scene.collection.objects.link(ground)
    ground.data.materials.append(felt_material())

    block_material(0)
    return scene


def block_material(tone):
    """A restrained procedural hardwood: bands along the length, distorted a
    little, between two close tones; matte."""
    name = "TimberWood%d" % tone
    mat = bpy.data.materials.get(name)
    if mat:
        return mat
    face, grain = TONES[tone]
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    nodes.clear()
    out = nodes.new("ShaderNodeOutputMaterial")
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.inputs["Roughness"].default_value = 0.68
    if "Specular IOR Level" in bsdf.inputs:
        bsdf.inputs["Specular IOR Level"].default_value = 0.25
    links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])

    coord = nodes.new("ShaderNodeTexCoord")
    mapping = nodes.new("ShaderNodeMapping")
    # Grain runs along the block's length: bands across x are stretched in x.
    mapping.inputs["Scale"].default_value = (6.0, 90.0, 40.0)
    links.new(coord.outputs["Object"], mapping.inputs["Vector"])
    noise = nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 1.0
    noise.inputs["Detail"].default_value = 3.0
    noise.inputs["Roughness"].default_value = 0.55
    links.new(mapping.outputs["Vector"], noise.inputs["Vector"])
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.35
    ramp.color_ramp.elements[0].color = srgb_to_linear(grain) + (1.0,)
    ramp.color_ramp.elements[1].position = 0.65
    ramp.color_ramp.elements[1].color = srgb_to_linear(face) + (1.0,)
    links.new(noise.outputs["Fac"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], bsdf.inputs["Base Color"])
    return mat


def felt_material():
    name = "TimberFelt"
    mat = bpy.data.materials.get(name)
    if mat:
        return mat
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = srgb_to_linear(FELT_RGB) + (1.0,)
    bsdf.inputs["Roughness"].default_value = 1.0
    if "Specular IOR Level" in bsdf.inputs:
        bsdf.inputs["Specular IOR Level"].default_value = 0.0
    return mat


def make_block(scene, name, tone=0):
    """One block, origin at its far-bottom corner (min x, min y, z = 0), so
    the sprite's anchor is the projection of the object origin."""
    mesh = bpy.data.meshes.new(name)
    l, w, h = BLOCK_L, BLOCK_W, BLOCK_H
    verts = [(0, 0, 0), (l, 0, 0), (l, w, 0), (0, w, 0), (0, 0, h), (l, 0, h), (l, w, h), (0, w, h)]
    faces = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    mesh.from_pydata(verts, [], faces)
    obj = bpy.data.objects.new(name, mesh)
    scene.collection.objects.link(obj)
    obj.data.materials.append(block_material(tone))
    bevel = obj.modifiers.new("Bevel", "BEVEL")
    bevel.width = BEVEL
    bevel.segments = 3
    bevel.limit_method = "ANGLE"
    bevel.harden_normals = True
    for p in mesh.polygons:
        p.use_smooth = True
    return obj


def place_block(obj, x, y, layer, along_x, tilt_deg=0.0):
    """Put a block at engine cell coordinates (widths) in a layer; a y block
    is the same object turned a quarter turn about its far-bottom corner."""
    obj.rotation_euler = (0.0, 0.0, 0.0 if along_x else math.radians(90.0))
    if along_x:
        obj.location = Vector((x * BLOCK_W, y * BLOCK_W, layer * BLOCK_H))
    else:
        # Rotated +90 about z, the block's length runs along +y from the
        # corner (x0 + w, y0): shift so its footprint starts at the cell.
        obj.location = Vector((x * BLOCK_W + BLOCK_W, y * BLOCK_W, layer * BLOCK_H))
    if tilt_deg:
        # A tumbling pose: tilted about its own long axis.
        axis = "X" if along_x else "Y"
        obj.rotation_euler.rotate_axis(axis, math.radians(tilt_deg))


def hide_all_blocks(scene):
    for o in scene.collection.objects:
        if o.name.startswith("TimberBlock"):
            o.hide_render = True
            o.hide_viewport = True


def set_res(scene, res):
    scene.render.resolution_x, scene.render.resolution_y = res
    # ortho_scale is the width of the view in metres.
    scene.camera.data.ortho_scale = res[0] / PX_PER_M


def render(scene, path):
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True, scene=scene.name)


# ---- exports ----------------------------------------------------------------

def load_pixels(path):
    """A PNG as a top-down (h, w, 4) uint8 array, raw values. Decoded here
    rather than through bpy.data.images, whose colour management would
    otherwise sit between the render and the sprite."""
    w, h, rows = read_png(path)
    return np.array(rows, dtype=np.uint8).reshape(h, w, 4)


def save_pixels(path, arr):
    """Write a top-down (h, w, 4) uint8 array as a PNG, raw values. Encoded
    here: Image.save() on a generated datablock writes the blank buffer, not
    the pixels set on it."""
    write_png(path, arr.astype(np.uint8))


def project(scene, point):
    """World point to top-down pixel coordinates in the current render."""
    from bpy_extras.object_utils import world_to_camera_view
    u, v, _ = world_to_camera_view(scene, scene.camera, Vector(point))
    return u * scene.render.resolution_x, (1.0 - v) * scene.render.resolution_y


def export_sprite(scene, src, dst, anchor_world):
    """Mirror, crop to the alpha bounds with PAD, and record the anchor: the
    pixel the engine's far-bottom corner of the block lands on."""
    arr = load_pixels(src)
    ax, ay = project(scene, anchor_world)
    arr = arr[:, ::-1]
    ax = arr.shape[1] - ax
    alpha = arr[:, :, 3] > 8
    ys, xs = np.where(alpha)
    x0, x1 = max(int(xs.min()) - PAD, 0), min(int(xs.max()) + PAD + 1, arr.shape[1])
    y0, y1 = max(int(ys.min()) - PAD, 0), min(int(ys.max()) + PAD + 1, arr.shape[0])
    crop = arr[y0:y1, x0:x1]
    save_pixels(dst, crop)
    return {
        "file": os.path.basename(dst),
        "w": int(x1 - x0),
        "h": int(y1 - y0),
        "ax": round(ax - x0, 2),
        "ay": round(ay - y0, 2),
        "bbox_px": [int(xs.max() - xs.min() + 1), int(ys.max() - ys.min() + 1)],
    }


def render_block_sprite(scene, tone, along_x, pose, anchors):
    name = "block_%s_t%d_p%d" % ("x" if along_x else "y", tone, pose)
    hide_all_blocks(scene)
    obj = bpy.data.objects.get("TimberBlockSprite") or make_block(scene, "TimberBlockSprite")
    obj.hide_render = False
    obj.hide_viewport = False
    obj.data.materials[0] = block_material(tone)
    place_block(obj, 0, 0, 0, along_x, POSES[pose])
    ground = bpy.data.objects.get("TimberGround")
    ground.hide_render = True
    set_res(scene, BLOCK_RES)
    # Centre the camera on the block so the render never clips it.
    centre = Vector((BLOCK_L / 2, BLOCK_W / 2, BLOCK_H / 2)) if along_x else \
             Vector((BLOCK_W / 2, BLOCK_L / 2, BLOCK_H / 2))
    aim_camera(scene, centre)
    raw = RENDERED + "/raw_%s.png" % name
    render(scene, raw)
    # The anchor is the engine's far-bottom corner of the cell, whatever the
    # pose: the object origin for an x block, (0, 0, 0) of the cell for y.
    corner = Vector((0.0, 0.0, 0.0))
    anchors[name] = export_sprite(scene, raw, RENDERED + "/%s.png" % name, corner)
    os.remove(raw)
    obj.hide_render = True
    obj.hide_viewport = True
    return anchors[name]


def aim_camera(scene, target):
    """Move the orthographic camera along its own axis so target is centred.
    The forward vector comes from the rotation itself: matrix_world is not
    refreshed until the depsgraph runs, which in a script is too late."""
    cam = scene.camera
    forward = cam.rotation_euler.to_matrix() @ Vector((0.0, 0.0, -1.0))
    cam.location = Vector(target) - forward * 1.0
    try:
        scene.view_layers[0].update()
    except Exception:
        pass


# ---- the felt and the shadow (numpy, no render) -----------------------------

def tileable_noise(size, cells, seed, octaves=3):
    """Value noise that wraps, so the tile repeats without a seam."""
    rng = np.random.default_rng(seed)
    out = np.zeros((size, size), dtype=np.float64)
    amp = 1.0
    total = 0.0
    for o in range(octaves):
        n = cells * (2 ** o)
        grid = rng.random((n, n))
        ys = (np.arange(size) / size) * n
        xs = ys.copy()
        y0 = np.floor(ys).astype(int) % n
        x0 = np.floor(xs).astype(int) % n
        fy = (ys - np.floor(ys))[:, None]
        fx = (xs - np.floor(xs))[None, :]
        fy = fy * fy * (3 - 2 * fy)
        fx = fx * fx * (3 - 2 * fx)
        y1 = (y0 + 1) % n
        x1 = (x0 + 1) % n
        g00 = grid[y0][:, x0]
        g01 = grid[y0][:, x1]
        g10 = grid[y1][:, x0]
        g11 = grid[y1][:, x1]
        v = (g00 * (1 - fx) + g01 * fx) * (1 - fy) + (g10 * (1 - fx) + g11 * fx) * fy
        out += (v - 0.5) * amp
        total += amp
        amp *= 0.5
    return out / total


def make_felt(size=128, seed=20260906):
    """Dark muted green with a low, fibrous, wrapping texture."""
    base = np.array(FELT_RGB, dtype=np.float64)
    fibre = tileable_noise(size, 16, seed, octaves=3)
    # Streaks: the same noise stretched along one axis, very faint.
    streak = np.roll(tileable_noise(size, 4, seed + 1, octaves=2), 0, axis=0)
    streak = np.repeat(streak[:, ::4], 4, axis=1)[:, :size]
    value = fibre * 10.0 + streak * 4.0
    rgb = np.clip(base[None, None, :] + value[:, :, None], 0, 255)
    arr = np.concatenate([rgb, np.full((size, size, 1), 255.0)], axis=2).astype(np.uint8)
    save_pixels(RENDERED + "/felt_tile.png", arr)
    return {"file": "felt_tile.png", "w": size, "h": size}


def make_ground_shadow():
    """A soft contact shadow under the tower's footprint: the base diamond in
    screen space, grown half a width, with a smooth falloff. Alpha only."""
    a = SCALE_PX
    grow = 0.5
    # The footprint's corners in screen space relative to the far corner.
    pts = np.array([(0, 0), (3 * a, 1.5 * a), (0, 3 * a), (-3 * a, 1.5 * a)], dtype=np.float64)
    pad = int(grow * a * 2 + 8)
    minx, maxx = pts[:, 0].min() - pad, pts[:, 0].max() + pad
    miny, maxy = pts[:, 1].min() - pad, pts[:, 1].max() + pad
    w, h = int(math.ceil(maxx - minx)), int(math.ceil(maxy - miny))
    yy, xx = np.mgrid[0:h, 0:w]
    px = xx + minx
    py = yy + miny
    # Signed distance to the diamond: |x|/3a + |y - 1.5a|/1.5a in diamond norm.
    d = np.abs(px) / (3 * a) + np.abs(py - 1.5 * a) / (1.5 * a)
    edge = (d - 1.0) * 1.5 * a          # px outside the footprint, roughly
    alpha = np.clip(1.0 - edge / (grow * a * 2), 0.0, 1.0)
    alpha = alpha * alpha * (3 - 2 * alpha)
    arr = np.zeros((h, w, 4), dtype=np.uint8)
    arr[:, :, 3] = (alpha * 150).astype(np.uint8)
    save_pixels(RENDERED + "/tower_shadow.png", arr)
    return {"file": "tower_shadow.png", "w": w, "h": h, "ax": round(-minx, 2), "ay": round(-miny, 2)}


# ---- the study and the composition ------------------------------------------

def build_tower(scene, layers, thin=()):
    """A tower of blocks for a picture, tones from the block index."""
    objs = []
    idx = 0
    for layer in range(layers):
        along_x = layer % 2 == 0
        for slot in range(3):
            if (layer, slot) in thin:
                idx += 1
                continue
            obj = make_block(scene, "TimberBlockTower%03d" % idx, tone=(idx * 7) % 3)
            if along_x:
                place_block(obj, 0, slot, layer, True)
            else:
                place_block(obj, slot, 0, layer, False)
            objs.append(obj)
            idx += 1
    return objs


def clear_tower(scene):
    for o in list(scene.collection.objects):
        if o.name.startswith("TimberBlockTower"):
            mesh = o.data
            bpy.data.objects.remove(o)
            bpy.data.meshes.remove(mesh)


def render_study(scene):
    """The same six-layer tower at three elevations, mirrored like the
    sprites, for the projection comparison."""
    ground = bpy.data.objects.get("TimberGround")
    ground.hide_render = False
    build_tower(scene, 6, thin=((1, 0), (3, 2)))
    out = {}
    cam = scene.camera
    for elev in (30.0, 35.26, 42.0):
        cam.rotation_euler = (math.radians(90.0 - elev), 0.0, math.radians(180.0 - AZIMUTH))
        set_res(scene, STUDY_RES)
        aim_camera(scene, Vector((1.5 * BLOCK_W, 1.5 * BLOCK_W, 3 * BLOCK_H)))
        raw = STUDY + "/raw.png"
        render(scene, raw)
        arr = load_pixels(raw)[:, ::-1]
        dst = STUDY + "/projection-%02d.png" % int(round(elev))
        save_pixels(dst, arr)
        os.remove(raw)
        out[dst] = {"layer_px": 0.6 * math.sqrt(2) * math.cos(math.radians(elev)) * SCALE_PX,
                    "top_shear_px": math.sin(math.radians(elev)) * SCALE_PX}
    cam.rotation_euler = (math.radians(90.0 - ELEVATION), 0.0, math.radians(180.0 - AZIMUTH))
    clear_tower(scene)
    return out


def render_composition(scene):
    """A reference picture: a played tower on the felt under the canonical
    camera and light, the look the sprites should reproduce when stacked."""
    ground = bpy.data.objects.get("TimberGround")
    ground.hide_render = False
    build_tower(scene, 20, thin=((0, 0), (2, 1), (4, 2), (5, 0), (7, 1), (9, 0), (11, 2), (13, 1),
                                 (15, 0), (19, 1), (19, 2)))
    set_res(scene, COMP_RES)
    aim_camera(scene, Vector((1.5 * BLOCK_W, 1.5 * BLOCK_W, 8.5 * BLOCK_H)))
    raw = STUDY + "/raw.png"
    render(scene, raw)
    arr = load_pixels(raw)[:, ::-1]
    save_pixels(STUDY + "/composition-reference.png", arr)
    os.remove(raw)
    clear_tower(scene)


def save_blend(scene):
    path = SRC + "/timber-art.blend"
    bpy.data.libraries.write(path, {scene}, fake_user=True)
    return path


# ---- entry ------------------------------------------------------------------

def run(stage):
    ensure_dirs()
    scene = get_scene()
    report = {"stage": stage, "px_per_m": PX_PER_M}
    if stage == "scene":
        report["blend"] = save_blend(scene)
        return report
    anchors = {}
    poses = [0] if stage == "proof" else list(POSES)
    tones = [0] if stage == "proof" else list(TONES)
    for tone in tones:
        for pose in poses:
            for along_x in (True, False):
                render_block_sprite(scene, tone, along_x, pose, anchors)
    anchors["felt"] = make_felt()
    anchors["shadow"] = make_ground_shadow()
    with open(RENDERED + "/anchors.json", "w", newline="\n") as f:
        json.dump({"scale_px": SCALE_PX, "layer_px": LAYER_PX, "sprites": anchors}, f, indent=2)
    report["sprites"] = anchors
    report["study"] = render_study(scene)
    render_composition(scene)
    report["blend"] = save_blend(scene)
    return report


if __name__ == "__main__" or "TIMBER_STAGE" in globals():
    _stage = globals().get("TIMBER_STAGE")
    if _stage is None:
        _argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
        _stage = _argv[0] if _argv else "proof"
    result = run(_stage)

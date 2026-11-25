#!/usr/bin/env python3
"""RavenEngine sample project generator.

Generates a small, fully self-contained sample project under sample/:

  sample/assets/meshes/sample_crate.obj     "Low-poly supply crate" mesh
  sample/assets/textures/crate_albedo.png   256x256 procedural crate texture
  sample/assets/textures/crate_normal.png   256x256 matching normal map
  sample/assets/materials/sample_crate.rmat PBR material referencing the textures
  sample/assets/scripts/spin.lua             rotates whatever entity it's attached to
  sample/assets/audio/chime.wav              short 1s synth chime (44.1kHz 16-bit PCM)
  sample/assets/**/*.meta                    sidecar metadata with STABLE uuids
  sample/scenes/sample_scene.ravenscene      the demo scene (lights + crate + camera)
  sample/project.ravenproject                project metadata pointer

The UUIDs are fixed on purpose so the scene can reference assets by UUID without a
two-pass import. Re-running the script regenerates byte-identical content (except the
WAV timestamps, which are not stored — output is deterministic).
"""

import json
import math
import os
import random
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAMPLE = os.path.join(ROOT, "sample")
ASSETS = os.path.join(SAMPLE, "assets")
SCENES = os.path.join(SAMPLE, "scenes")

# Stable UUIDs (uuid4-shaped, hand-picked constants)
UUID_MESH_CRATE = "3f6b9c2a-1a7e-4c1d-9b2f-8d5a6e7f1023"
UUID_TEX_ALBEDO = "a41c7b09-5e2d-4b8f-b3d6-2c8f1a0e91b7"
UUID_TEX_NORMAL = "b52d8c1a-6f3e-4a9c-c4e7-3d9a2b1f02c8"
UUID_MAT_CRATE = "c63e9d2b-7a4f-4b0d-d5f8-4e0b3c2a13d9"
UUID_SCRIPT_SPIN = "d74f0e3c-8b5a-4c1e-e6a9-5f1c4d3b24e0"
UUID_AUDIO_CHIME = "e8501f4d-9c6b-4d2f-f7b0-6a2d5e4c35f1"


def write_bytes(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    print("wrote", os.path.relpath(path, ROOT), len(data), "bytes")


def write_text(path, text):
    write_bytes(path, text.encode("utf-8"))


def write_png(path, width, height, rgba):
    """Minimal PNG writer: 8-bit RGBA, filter type 0, zlib-compressed scanlines."""
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter: none
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
           chunk(b"IEND", b""))
    write_bytes(path, png)


def gen_crate_albedo(path):
    w = h = 256
    rnd = random.Random(20261125)
    px = bytearray(w * h * 4)

    def put(x, y, r, g, b, a=255):
        i = (y * w + x) * 4
        px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a

    # Plank rows with alternating base tones + grain noise.
    plank_h = 32
    tones = [(138, 90, 43), (150, 99, 47), (128, 82, 39), (158, 106, 52)]
    for y in range(h):
        plank = (y // plank_h) % len(tones)
        base = tones[plank]
        jitter = rnd.randint(-14, 14)
        for x in range(w):
            grain = rnd.randint(-10, 10)
            # seam between planks
            if y % plank_h < 2:
                r, g, b = 74, 48, 22
            else:
                r = max(0, min(255, base[0] + jitter + grain))
                g = max(0, min(255, base[1] + jitter + grain))
                b = max(0, min(255, base[2] + jitter + grain))
            put(x, y, r, g, b)

    # Frame border (crate edge)
    def frame_rect(x0, y0, x1, y1, thickness, color):
        for y in range(y0, y1):
            for x in range(x0, x1):
                if x < x0 + thickness or x >= x1 - thickness or y < y0 + thickness or y >= y1 - thickness:
                    put(x, y, *color)

    frame_rect(4, 4, w - 4, h - 4, 18, (94, 62, 28))

    # Diagonal cross brace
    for t in range(0, w):
        for d in range(-7, 8):
            y = t + d
            if 0 <= y < h:
                i = (y * w + t) * 4
                px[i] = min(255, int(px[i] * 0.72)); px[i + 1] = min(255, int(px[i + 1] * 0.72)); px[i + 2] = min(255, int(px[i + 2] * 0.72))

    # Corner bolts
    for cx, cy in [(26, 26), (w - 26, 26), (26, h - 26), (w - 26, h - 26)]:
        for y in range(-9, 10):
            for x in range(-9, 10):
                d2 = x * x + y * y
                if d2 <= 81:
                    px_ = cx + x; py_ = cy + y
                    if 0 <= px_ < w and 0 <= py_ < h:
                        if d2 <= 20:
                            put(px_, py_, 210, 210, 215)
                        elif d2 <= 56:
                            put(px_, py_, 120, 120, 128)
                        else:
                            put(px_, py_, 70, 70, 76)

    write_png(path, w, h, px)


def gen_crate_normal(path):
    w = h = 256
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            # Flat normal with slight bevel around the frame/bolt ring.
            nx, ny = 0.0, 0.0
            border = 4 + 18
            if x < border or x >= w - border or y < border or y >= h - border:
                # bevel: tilt towards the nearest edge
                dx = min(x, w - 1 - x) - border
                dy = min(y, h - 1 - y) - border
                if dx < 0: nx = math.copysign(0.55, x - (w - 1 - x))
                if dy < 0: ny = math.copysign(0.55, y - (h - 1 - y))
            nz = math.sqrt(max(0.0, 1.0 - nx * nx - ny * ny))
            i = (y * w + x) * 4
            px[i] = int((nx * 0.5 + 0.5) * 255)
            px[i + 1] = int((ny * 0.5 + 0.5) * 255)
            px[i + 2] = int(nz * 255)
            px[i + 3] = 255
    write_png(path, w, h, px)


def gen_cube_obj(path, half=1.0, name="cube"):
    """24-vertex cube (per-face normals/uvs)."""
    faces = [
        # +Y up (top)
        ([(-1, 1, -1), (1, 1, -1), (1, 1, 1), (-1, 1, 1)], (0, 1, 0)),
        # -Y down (bottom)
        ([(-1, -1, 1), (1, -1, 1), (1, -1, -1), (-1, -1, -1)], (0, -1, 0)),
        # +Z front
        ([(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)], (0, 0, 1)),
        # -Z back
        ([(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)], (0, 0, -1)),
        # +X right
        ([(1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)], (1, 0, 0)),
        # -X left
        ([(-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)], (-1, 0, 0)),
    ]
    lines = [f"# RavenEngine sample asset — {name}", f"o {name}"]
    uvs = [(0, 0), (1, 0), (1, 1), (0, 1)]
    v_base = 1   # 1-based vertex index of first vertex of current face
    vt_base = 1  # 1-based texcoord index of first texcoord of current face
    vn_index = 1 # 1-based normal index of current face
    for verts, n in faces:
        for v in verts:
            lines.append(f"v {v[0]*half:.6f} {v[1]*half:.6f} {v[2]*half:.6f}")
        for uv in uvs:
            lines.append(f"vt {uv[0]:.6f} {uv[1]:.6f}")
        lines.append(f"vn {n[0]:.6f} {n[1]:.6f} {n[2]:.6f}")
        quad = []
        for k in range(4):
            quad.append(f"{v_base+k}/{vt_base+k}/{vn_index}")
        lines.append("f " + " ".join(quad))
        v_base += 4
        vt_base += 4
        vn_index += 1
    write_text(path, "\n".join(lines) + "\n")


def gen_chime_wav(path):
    """1.2s two-note chime: 660 Hz decay + 990 Hz sparkle over a soft 1320 Hz bed."""
    rate = 44100
    seconds = 1.2
    n = int(rate * seconds)
    frames = bytearray()
    for i in range(n):
        t = i / rate
        env1 = math.exp(-3.2 * t)
        env2 = math.exp(-6.0 * t)
        s = (0.55 * env1 * math.sin(2 * math.pi * 660 * t) +
             0.30 * env2 * math.sin(2 * math.pi * 990 * t) +
             0.15 * env2 * math.sin(2 * math.pi * 1320 * t))
        s = max(-1.0, min(1.0, s * 0.8))
        frames += struct.pack("<h", int(s * 32767))
    data_size = len(frames)
    riff = (b"RIFF" + struct.pack("<I", 36 + data_size) + b"WAVE" +
            b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) +
            b"data" + struct.pack("<I", data_size) + bytes(frames))
    write_bytes(path, riff)


def meta(path, uuid, asset_type, import_settings):
    payload = {
        "uuid": uuid,
        "assetType": asset_type,
        "relativePath": os.path.relpath(path, os.path.join(SAMPLE, "assets")).replace(os.sep, "/"),
        "importVersion": 1,
        "importSettings": import_settings,
    }
    write_text(path + ".meta", json.dumps(payload, indent=2) + "\n")


def main():
    for sub in ("meshes", "textures", "materials", "scripts", "audio"):
        os.makedirs(os.path.join(ASSETS, sub), exist_ok=True)
    os.makedirs(SCENES, exist_ok=True)

    obj_path = os.path.join(ASSETS, "meshes", "sample_crate.obj")
    gen_cube_obj(obj_path, half=1.0, name="sample_crate")

    albedo = os.path.join(ASSETS, "textures", "crate_albedo.png")
    gen_crate_albedo(albedo)
    normal = os.path.join(ASSETS, "textures", "crate_normal.png")
    gen_crate_normal(normal)

    material = {
        "albedo": [0.92, 0.78, 0.58, 1.0],
        "metallic": 0.06,
        "roughness": 0.58,
        "albedoTexture": UUID_TEX_ALBEDO,
        "normalTexture": UUID_TEX_NORMAL,
        "emissive": [0.0, 0.0, 0.0],
        "normalScale": 1.0,
    }
    mat_path = os.path.join(ASSETS, "materials", "sample_crate.rmat")
    write_text(mat_path, json.dumps(material, indent=2) + "\n")

    lua_path = os.path.join(ASSETS, "scripts", "spin.lua")
    write_text(lua_path, """-- RavenEngine sample script: spin the owning entity around Y.
-- Demonstrates: per-entity script instances + Raven.* bindings.
local speed = 45.0  -- degrees per second

function onStart(entity)
    Raven.log("spin.lua started on entity " .. tostring(entity))
end

function onUpdate(entity, dt)
    local rx, ry, rz = Raven.getRotation(entity)
    Raven.setRotation(entity, rx, ry + math.rad(speed) * dt, rz)
end
""")

    wav_path = os.path.join(ASSETS, "audio", "chime.wav")
    gen_chime_wav(wav_path)

    # Sidecar metadata with stable UUIDs.
    meta(obj_path, UUID_MESH_CRATE, "mesh", {"flipUVs": True})
    meta(albedo, UUID_TEX_ALBEDO, "texture", {"srgb": True, "generateMipmaps": True})
    meta(normal, UUID_TEX_NORMAL, "texture", {"srgb": False, "generateMipmaps": True})
    meta(mat_path, UUID_MAT_CRATE, "material", {})
    meta(lua_path, UUID_SCRIPT_SPIN, "luaScript", {})
    meta(wav_path, UUID_AUDIO_CHIME, "audio", {})

    # Demo scene (references the stable UUIDs above).
    def vec(x, y, z):
        return [x, y, z]

    entities = [
        {
            "id": 0,
            "components": {
                "name": {"name": "Sun"},
                "transform": {"position": vec(0, 6, 0), "rotation": vec(0, 0, 0), "scale": vec(1, 1, 1), "parent": -1},
                "directionalLight": {"direction": vec(0.42, -0.83, 0.36), "color": vec(1.0, 0.96, 0.88),
                                     "intensity": 3.0, "castShadow": True},
            },
        },
        {
            "id": 1,
            "components": {
                "name": {"name": "Warm Point Light"},
                "transform": {"position": vec(-2.2, 1.4, 1.6), "rotation": vec(0, 0, 0), "scale": vec(1, 1, 1), "parent": -1},
                "pointLight": {"color": vec(1.0, 0.62, 0.25), "intensity": 12.0, "radius": 7.5},
            },
        },
        {
            "id": 2,
            "components": {
                "name": {"name": "Crate"},
                "transform": {"position": vec(0, 1.0, 0), "rotation": vec(0, 0.5, 0), "scale": vec(1, 1, 1), "parent": -1},
                "meshRenderer": {"mesh": UUID_MESH_CRATE, "material": UUID_MAT_CRATE, "castShadow": True, "visible": True},
                "script": {"scriptPath": "assets/scripts/spin.lua", "enabled": True},
                "boxCollider": {"halfExtents": vec(1, 1, 1)},
                "audioSource": {"clip": UUID_AUDIO_CHIME, "volume": 0.6, "loop": False, "playOnStart": False, "is3D": True},
            },
        },
        {
            "id": 3,
            "components": {
                "name": {"name": "Ground"},
                "transform": {"position": vec(0, -0.15, 0), "rotation": vec(0, 0, 0), "scale": vec(12, 0.1, 12), "parent": -1},
                "meshRenderer": {"mesh": UUID_MESH_CRATE, "material": UUID_MAT_CRATE, "castShadow": False, "visible": True},
                "boxCollider": {"halfExtents": vec(1, 1, 1)},
                "rigidbody": {"type": "static", "mass": 1.0, "linearVelocity": vec(0, 0, 0), "useGravity": False, "restitution": 0.0},
            },
        },
        {
            "id": 4,
            "components": {
                "name": {"name": "Main Camera"},
                "transform": {"position": vec(4.2, 3.1, 6.3), "rotation": vec(-0.42, 0.55, 0), "scale": vec(1, 1, 1), "parent": -1},
                "camera": {"fovDegrees": 55.0, "nearPlane": 0.1, "farPlane": 250.0, "isPrimary": True},
            },
        },
    ]
    scene = {"schemaVersion": 1, "name": "Sample Scene", "entities": entities}
    write_text(os.path.join(SCENES, "sample_scene.ravenscene"), json.dumps(scene, indent=2) + "\n")

    project = {
        "name": "RavenEngine Sample",
        "projectVersion": 1,
        "assetsRoot": "assets",
        "scenes": ["scenes/sample_scene.ravenscene"],
        "defaultScene": "scenes/sample_scene.ravenscene",
    }
    write_text(os.path.join(SAMPLE, "project.ravenproject"), json.dumps(project, indent=2) + "\n")

    print("\nSample project generated under", os.path.relpath(SAMPLE, ROOT))


if __name__ == "__main__":
    sys.exit(main())

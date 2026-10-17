"""GPU integration checks for glTF transmission/volume. Uses only Python's standard library.

Run: python tests/test_transmission.py --exe build/snr.exe
Fixtures and renders are generated in a temporary directory; no external assets are required.
"""
import argparse
import base64
import copy
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib


def image_uri():
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    # R=1/G=0 at u=.25, R=0/G=1 at u=.75. Nearest filtering makes the channel test exact.
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 1, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes([0, 255, 0, 0, 255, 0, 255, 0, 255])))
    png += chunk(b'IEND', b'')
    return 'data:image/png;base64,' + base64.b64encode(png).decode()


def material(thickness=0, color=(1, 1, 1), distance=None, ior=1.5, roughness=0):
    volume = {'thicknessFactor': thickness, 'attenuationColor': list(color)}
    if distance is not None:
        volume['attenuationDistance'] = distance
    return {'pbrMetallicRoughness': {'baseColorFactor': [1, 1, 1, 1], 'metallicFactor': 0,
                                    'roughnessFactor': roughness},
            'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1},
                           'KHR_materials_volume': volume, 'KHR_materials_ior': {'ior': ior}}}


def slab(size=4, front=.5, back=-.5):
    s, f, b = size, front, back
    return [[(-s,-s,f),(s,-s,f),(s,s,f),(-s,s,f)],
            [(s,-s,b),(-s,-s,b),(-s,s,b),(s,s,b)],
            [(s,-s,f),(s,-s,b),(s,s,b),(s,s,f)],
            [(-s,-s,b),(-s,-s,f),(-s,s,f),(-s,s,b)],
            [(-s,s,f),(s,s,f),(s,s,b),(-s,s,b)],
            [(-s,-s,b),(s,-s,b),(s,-s,f),(-s,-s,f)]]


def fixture(materials, meshes):
    data = bytearray()
    model = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': []}],
             'nodes': [], 'meshes': [], 'materials': materials, 'bufferViews': [], 'accessors': [],
             'images': [{'uri': image_uri()}], 'samplers': [{'magFilter': 9728, 'minFilter': 9728}],
             'textures': [{'source': 0, 'sampler': 0}],
             'extensionsUsed': ['KHR_materials_transmission', 'KHR_materials_volume', 'KHR_materials_ior', 'KHR_texture_transform'],
             'extensionsRequired': ['KHR_materials_transmission', 'KHR_materials_volume', 'KHR_materials_ior']}

    def accessor(values, components, component_type):
        packed = struct.pack('<' + ('f' if component_type == 5126 else 'I') * len(values), *values)
        view = len(model['bufferViews'])
        model['bufferViews'].append({'buffer': 0, 'byteOffset': len(data), 'byteLength': len(packed)})
        data.extend(packed)
        index = len(model['accessors'])
        model['accessors'].append({'bufferView': view, 'componentType': component_type,
                                   'count': len(values) // components,
                                   'type': {1: 'SCALAR', 2: 'VEC2', 3: 'VEC3'}[components]})
        return index

    for material_index, quads in meshes:
        points = [p for quad in quads for p in quad]
        positions = accessor([v for p in points for v in p], 3, 5126)
        uv0 = accessor([.25, .5] * len(points), 2, 5126)
        uv1 = accessor([.75, .5] * len(points), 2, 5126)
        indices = accessor([4*q+i for q in range(len(quads)) for i in (0, 1, 2, 0, 2, 3)], 1, 5125)
        model['meshes'].append({'primitives': [{'attributes': {'POSITION': positions, 'TEXCOORD_0': uv0,
                                                              'TEXCOORD_1': uv1},
                                               'indices': indices, 'material': material_index}]})
        model['nodes'].append({'mesh': len(model['meshes'])-1})
        model['scenes'][0]['nodes'].append(len(model['nodes'])-1)
    model['buffers'] = [{'byteLength': len(data), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(data).decode()}]
    return model


def read_hdr(path):
    with path.open('rb') as stream:
        while stream.readline().strip():
            pass
        axis_y, h, axis_x, w = stream.readline().decode().split()
        assert (axis_y, axis_x) == ('-Y', '+X')
        width, height = int(w), int(h)
        pixels = []
        for _ in range(height):
            start = stream.read(4)
            assert start[:2] == b'\x02\x02' and int.from_bytes(start[2:], 'big') == width
            channels = []
            for _ in range(4):
                values = bytearray()
                while len(values) < width:
                    count = stream.read(1)[0]
                    if count > 128:
                        values.extend(stream.read(1) * (count-128))
                    else:
                        values.extend(stream.read(count))
                assert len(values) == width
                channels.append(values)
            for x in range(width):
                scale = math.ldexp(1., channels[3][x]-136) if channels[3][x] else 0.
                pixels.append(tuple(channels[c][x]*scale for c in range(3)))
        assert all(math.isfinite(v) and v >= 0 for rgb in pixels for v in rgb)
        return tuple(sum(rgb[c] for rgb in pixels)/len(pixels) for c in range(3))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=Path('build/snr.exe'))
    args = parser.parse_args()
    exe = args.exe.resolve()
    with tempfile.TemporaryDirectory(prefix='snr-transmission-') as directory:
        work = Path(directory)

        def run(name, model, raster=False, environment=1, camera=(0,0,3), target=(0,0,0)):
            source = work / (name+'.gltf')
            source.write_text(json.dumps(model), encoding='utf-8')
            output = work / (name+'.png')
            command = [str(exe), '--model', str(source), '--width', '16', '--height', '16', '--spp', '256',
                       '--batch-size', '64', '--max-bounces', '12', '--no-nee', '--environment', str(environment),
                       '--fov', '1', '--camera', *map(str,camera), '--target', *map(str,target),
                       '--output', str(output), '--validate']
            if raster:
                command.append('--raster')
            result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=120)
            assert result.returncode == 0, result.stdout + result.stderr
            assert 'Unsupported' not in result.stderr and 'Ignoring unsupported' not in result.stderr, result.stderr
            rgb = read_hdr(output.with_suffix('.hdr'))
            print(name, tuple(round(v,4) for v in rgb), flush=True)
            return rgb

        def near(actual, expected, tolerance=.015):
            assert all(abs(a-e) <= tolerance for a,e in zip(actual,expected)), (actual, expected)

        # A slab with index-matched interfaces isolates Beer-Lambert absorption and true path length.
        absorbing = fixture([material(1, (.25,.5,.75), 1, 1)], [(0,slab())])
        near(run('volume-absorption', absorbing), (.25,.5,.75))
        near(run('volume-absorption-raster', absorbing, raster=True), (.25,.5,.75))
        black = fixture([material(1, (0,.5,.75), 1, 1)], [(0,slab())])
        near(run('zero-attenuation-channel', black), (0,.5,.75))
        thin = copy.deepcopy(absorbing)
        thin['materials'][0]['extensions']['KHR_materials_volume']['thicknessFactor'] = 0
        near(run('thin-no-volume-absorption', thin), (1,1,1))
        infinite = fixture([material(1, (.25,.5,.75), ior=1)], [(0,slab())])
        near(run('infinite-attenuation-distance', infinite), (1,1,1))
        baked = copy.deepcopy(absorbing)
        baked['materials'][0]['extensions']['KHR_materials_volume']['thicknessTexture'] = {'index': 0}
        near(run('ignore-baked-thickness-distance', baked), (.25,.5,.75))

        # Nested media replace rather than add absorption while inside the inner volume.
        nested = fixture([material(1,(.8,.8,.8),1,1), material(1,(.5,.25,1),1,1)],
                         [(0,slab(front=1,back=-1)),(1,slab(size=3))])
        near(run('nested-volumes', nested), (.4,.2,.8))
        near(run('nested-volumes-raster', nested, raster=True), (.4,.2,.8))
        near(run('clear-refraction-energy', fixture([material(1)], [(0,slab())])), (1,1,1), .035)

        # A camera inside a broad slab looks above the critical angle; all paths are trapped by TIR.
        trapped = fixture([material(1)], [(0,slab(size=100))])
        near(run('total-internal-reflection', trapped, camera=(0,0,0), target=(1,0,.5)), (0,0,0))

        # An emissive backdrop distinguishes optical transmission from opaque coverage.
        emitter = {'pbrMetallicRoughness': {'metallicFactor': 0}, 'emissiveFactor': [1,0,0]}
        sheet = [[(-4,-4,0),(4,-4,0),(4,4,0),(-4,4,0)]]
        backdrop = [[(-10,-10,-2),(10,-10,-2),(10,10,-2),(-10,10,-2)]]
        textured = material()
        textured['extensions']['KHR_materials_transmission']['transmissionTexture'] = {'index': 0, 'texCoord': 1}
        opaque_texel = fixture([textured,emitter],[(0,sheet),(1,backdrop)])
        near(run('transmission-red-channel-zero', opaque_texel, environment=0), (0,0,0))
        transmitting = copy.deepcopy(opaque_texel)
        texture = transmitting['materials'][0]['extensions']['KHR_materials_transmission']['transmissionTexture']
        texture['extensions'] = {'KHR_texture_transform': {'texCoord': 1, 'offset': [-.5,0]}}
        red = run('transmission-uv-transform', transmitting, environment=0)
        assert .90 < red[0] <= 1.02 and red[1] < .001 and red[2] < .001, red
        red = run('transmission-uv-transform-raster', transmitting, raster=True, environment=0)
        assert .90 < red[0] <= 1.02 and red[1] < .001 and red[2] < .001, red
        # Refraction through parallel interfaces causes a measurable lateral displacement.
        blue_emitter = copy.deepcopy(emitter)
        blue_emitter['emissiveFactor'] = [0,0,1]
        red_half = [[(-1.2,-10,-2),(10,-10,-2),(10,10,-2),(-1.2,10,-2)]]
        blue_half = [[(-10,-10,-2),(-1.2,-10,-2),(-1.2,10,-2),(-10,10,-2)]]
        refracting = fixture([material(1),emitter,blue_emitter],[(0,slab()),(1,red_half),(2,blue_half)])
        shifted = run('refractive-lateral-displacement', refracting, environment=0, camera=(2,0,3))
        assert shifted[0] > .8 and shifted[2] < .1, shifted
        shifted = run('refractive-lateral-displacement-raster', refracting, raster=True, environment=0, camera=(2,0,3))
        assert shifted[0] > .8 and shifted[2] < .1, shifted
        index_matched = copy.deepcopy(refracting)
        index_matched['materials'][0]['extensions']['KHR_materials_ior']['ior'] = 1
        unshifted = run('index-matched-straight-through', index_matched, environment=0, camera=(2,0,3))
        assert unshifted[2] > .9 and unshifted[0] < .1, unshifted
        metallic = copy.deepcopy(transmitting)
        metallic['materials'][0]['pbrMetallicRoughness']['metallicFactor'] = 1
        near(run('metals-do-not-transmit', metallic, environment=0), (0,0,0))
        rough = run('rough-glass-energy', fixture([material(1,roughness=.3)],[(0,slab())]))
        assert all(.7 < c < 1.08 for c in rough), rough
        print('All transmission/volume GPU integration checks passed.')


if __name__ == '__main__':
    main()

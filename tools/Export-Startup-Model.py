"""Export the designer's meshes with arc-length reveal coordinates.

Development only: Python 3 + rhino3dm 8.35.0. The app embeds the generated
header and has no Python, Rhino, network, or .NET runtime dependency.
"""
import argparse
import hashlib
import math
import sys
from pathlib import Path

local_modules = Path(__file__).resolve().parent / 'model-tools' / 'modules'
if local_modules.is_dir():
    sys.path.insert(0, str(local_modules))
import rhino3dm as rhino

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
model = rhino.File3dm.Read(str(args.source))
if model is None:
    raise RuntimeError('Cannot read the source model')
solids = [obj.Geometry for obj in model.Objects
          if isinstance(obj.Geometry, rhino.Brep)
          and model.Layers[obj.Attributes.LayerIndex].Visible]
if not solids or not all(solid.IsSolid for solid in solids):
    raise RuntimeError('Expected closed solids in the visible 3D layer')
boxes = [solid.GetBoundingBox() for solid in solids]
lo = [min(getattr(box.Min, axis) for box in boxes) for axis in 'XYZ']
hi = [max(getattr(box.Max, axis) for box in boxes) for axis in 'XYZ']
center = [(a + b) / 2 for a, b in zip(lo, hi)]
width = hi[0] - lo[0]
vertices = []
lookup = {}
triangles = []

def curve_points(curve, reverse=False):
    points = [curve.PointAt(curve.Domain.T0+(curve.Domain.T1-curve.Domain.T0)*i/1024)
              for i in range(1025)]
    if reverse:
        points.reverse()
    distances = [0.0]
    for a, b in zip(points, points[1:]):
        distances.append(distances[-1] + math.hypot(b.X-a.X, b.Z-a.Z))
    samples = []
    cursor = 0
    for i in range(257):
        goal = distances[-1]*i/256
        while cursor < len(points)-2 and distances[cursor+1] < goal:
            cursor += 1
        t = (goal-distances[cursor]) / max(1e-12,distances[cursor+1]-distances[cursor])
        a, b = points[cursor:cursor+2]
        samples.append((a.X+(b.X-a.X)*t, a.Z+(b.Z-a.Z)*t))
    return samples

# These are the exact paired boundaries of the designer's two visible arcs.
# The source checksum makes the semantic indices explicit, not a heuristic.
digest = hashlib.sha256(args.source.read_bytes()).hexdigest()
if digest != '1b8fc8c587ba87541c41cb879f99b15b157cf03d170a6465882690306aa3edcd':
    raise RuntimeError('Review the semantic arc/arrow mapping for a changed source model')
paths = {}
for part, solid_index, inner, outer in [(1,2,0,2), (2,3,7,9)]:
    a = curve_points(solids[solid_index].Edges[inner], reverse=True)
    b = curve_points(solids[solid_index].Edges[outer])
    centerline = [((p[0]+q[0])/2, (p[1]+q[1])/2) for p,q in zip(a,b)]
    distances = [0.0]
    for p,q in zip(centerline, centerline[1:]):
        distances.append(distances[-1]+math.hypot(q[0]-p[0],q[1]-p[1]))
    paths[part] = (centerline, distances)

def reveal_coordinate(point, part):
    if part not in paths:
        return 0.0
    path, distances = paths[part]
    best_distance, best_t = float('inf'), 0
    for i,(a,b) in enumerate(zip(path,path[1:])):
        dx,dz = b[0]-a[0],b[1]-a[1]
        t = max(0,min(1,((point.X-a[0])*dx+(point.Z-a[1])*dz)/max(1e-12,dx*dx+dz*dz)))
        distance = (point.X-a[0]-t*dx)**2+(point.Z-a[1]-t*dz)**2
        if distance < best_distance:
            best_distance = distance
            best_t = (distances[i]+t*(distances[i+1]-distances[i]))/distances[-1]
    return best_t

def index(point, part):
    # Original logo faces along Y. Preserve its X/Z silhouette, with screen Y
    # down and positive depth toward the viewer. Keep the original thickness.
    xyz = ((point.X - center[0]) / width,
           -(point.Z - center[2]) / width,
           (point.Y - center[1]) / width)
    key = tuple(round(value, 7) for value in (*xyz,reveal_coordinate(point,part)))
    if key not in lookup:
        lookup[key] = len(vertices)
        vertices.append(key)
    return lookup[key]

def cut_arrow(points, keep_arrow):
    # Plane through the two original shaft/arrow joins. Splitting the few
    # straddling triangles preserves the exact original rounded arrow outline.
    a = solids[3].Edges[0].PointAtStart
    b = solids[3].Edges[6].PointAtEnd
    def signed(p):
        value = (p.X-a.X)*(a.Z-b.Z)+(p.Z-a.Z)*(b.X-a.X)
        return -value if keep_arrow else value
    clipped = []
    for p,q in zip(points, points[1:]+points[:1]):
        dp,dq = signed(p),signed(q)
        if dp >= 0:
            clipped.append(p)
        if (dp >= 0) != (dq >= 0):
            t = dp/(dp-dq)
            clipped.append(rhino.Point3d(p.X+(q.X-p.X)*t,p.Y+(q.Y-p.Y)*t,p.Z+(q.Z-p.Z)*t))
    return clipped

for solid_index, solid in enumerate(solids):
    for face in solid.Faces:
        mesh = face.GetMesh(rhino.MeshType.Render)
        if mesh is None:
            raise RuntimeError('Missing designer-cached render mesh')
        for a, b, c, d in mesh.Faces:
            for tri in [(a, b, c)] + ([(a, c, d)] if c != d else []):
                points = [mesh.Vertices[i] for i in tri]
                parts = [(3,cut_arrow(points,True)),(2,cut_arrow(points,False))] if solid_index==3 else [(1 if solid_index==2 else 0,points)]
                for part, polygon in parts:
                    for i in range(1,len(polygon)-1):
                        ids = [index(p,part) for p in (polygon[0],polygon[i],polygon[i+1])]
                        p, q, r = [vertices[i] for i in ids]
                        u = [q[i] - p[i] for i in range(3)]
                        v = [r[i] - p[i] for i in range(3)]
                        n = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
                        if sum(x*x for x in n) > 1e-20:
                            triangles.append((*ids,part))
if len(vertices) >= 65536:
    raise RuntimeError('Mesh exceeds 16-bit index budget')
lines = ['// Generated by tools/Export-Startup-Model.py. Do not hand-edit.',
         '// Source SHA-256: ' + digest, '#pragma once', '#include <cstdint>',
         'namespace sempervirens::startup_model {',
         'struct Vertex { float x, y, z, reveal; };',
         'struct Triangle { std::uint16_t a, b, c, part; };',
         'inline constexpr Vertex vertices[] = {']
for vertex in vertices:
    lines.append('    {' + ', '.join(f'{value:.7f}f' for value in vertex) + '},')
lines.extend(['};', 'inline constexpr Triangle triangles[] = {'])
for triangle in triangles:
    lines.append('    {' + ', '.join(str(value) for value in triangle) + '},')
lines.extend(['};', '} // namespace sempervirens::startup_model', ''])
args.output.write_text('\n'.join(lines), encoding='utf-8')
print(f'{len(solids)} original solids; {len(vertices)} vertices; {len(triangles)} triangles; source {digest}')
print(f'Normalized thickness: {(hi[1]-lo[1])/width:.5f}; aspect: {width/(hi[2]-lo[2]):.4f}')

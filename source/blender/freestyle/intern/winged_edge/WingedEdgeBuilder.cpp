/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 * \brief Class to render a WingedEdge data structure from a polyhedral data structure organized in
 * nodes of a scene graph
 */

#include <set>

#include "WingedEdgeBuilder.h"

#include "../geometry/GeomUtils.h"

#include "../scene_graph/NodeShape.h"

using namespace std;

namespace Freestyle {

void WingedEdgeBuilder::visitIndexedFaceSet(IndexedFaceSet &ifs)
{
  if (_pRenderMonitor && _pRenderMonitor->testBreak()) {
    return;
  }
  WShape *shape = new WShape;
  if (!buildWShape(*shape, ifs)) {
    delete shape;
    return;
  }
  shape->setId(ifs.getId().getFirst());
  // ifs.setId(shape->GetId());
}

void WingedEdgeBuilder::visitNodeShape(NodeShape &ns)
{
  // Sets the current material to iShapeode->material:
  _current_frs_material = &(ns.frs_material());
}

void WingedEdgeBuilder::visitNodeTransform(NodeTransform &tn)
{
  if (!_current_matrix) {
    _current_matrix = new Matrix44r(tn.matrix());
    return;
  }

  _matrices_stack.push_back(_current_matrix);
  Matrix44r *new_matrix = new Matrix44r(*_current_matrix * tn.matrix());
  _current_matrix = new_matrix;
}

void WingedEdgeBuilder::visitNodeTransformAfter(NodeTransform &UNUSED(transform))
{
  delete _current_matrix;

  if (_matrices_stack.empty()) {
    _current_matrix = nullptr;
    return;
  }

  _current_matrix = _matrices_stack.back();
  _matrices_stack.pop_back();
}

bool WingedEdgeBuilder::buildWShape(WShape &shape, IndexedFaceSet &ifs)
{
  uint vsize = ifs.vsize();
  uint nsize = ifs.nsize();
  // soc unused - unsigned tsize = ifs.tsize();

  const float *vertices = ifs.vertices();
  const float *normals = ifs.normals();
  const float *texCoords = ifs.texCoords();

  float *new_vertices;
  float *new_normals;

  new_vertices = new float[vsize];
  new_normals = new float[nsize];

  // transform coordinates from local to world system
  if (_current_matrix) {
    transformVertices(vertices, vsize, *_current_matrix, new_vertices);
    transformNormals(normals, nsize, *_current_matrix, new_normals);
  }
  else {
    memcpy(new_vertices, vertices, vsize * sizeof(*new_vertices));
    memcpy(new_normals, normals, nsize * sizeof(*new_normals));
  }

  const IndexedFaceSet::TRIANGLES_STYLE *faceStyle = ifs.trianglesStyle();

  vector<FrsMaterial> frs_materials;
  if (ifs.msize()) {
    const FrsMaterial *const *mats = ifs.frs_materials();
    for (uint i = 0; i < ifs.msize(); ++i) {
      frs_materials.push_back(*(mats[i]));
    }
    shape.setFrsMaterials(frs_materials);
  }

#if 0
  const FrsMaterial *mat = (ifs.frs_material());
  if (mat) {
    shape.setFrsMaterial(*mat);
  }
  else if (_current_frs_material) {
    shape.setFrsMaterial(*_current_frs_material);
  }
#endif
  const IndexedFaceSet::FaceEdgeMark *faceEdgeMarks = ifs.faceEdgeMarks();

  // sets the current WShape to shape
  _current_wshape = &shape;

  // create a WVertex for each vertex
  buildWVertices(shape, new_vertices, vsize);

  const uint *vindices = ifs.vindices();
  const uint *nindices = ifs.nindices();
  const uint *tindices = nullptr;
  if (ifs.tsize()) {
    tindices = ifs.tindices();
  }

  const uint *mindices = nullptr;
  if (ifs.msize()) {
    mindices = ifs.mindices();
  }
  const uint *numVertexPerFace = ifs.numVertexPerFaces();
  const uint numfaces = ifs.numFaces();

  for (uint index = 0; index < numfaces; index++) {
    switch (faceStyle[index]) {
      case IndexedFaceSet::TRIANGLE_STRIP:
        buildTriangleStrip(new_vertices,
                           new_normals,
                           frs_materials,
                           texCoords,
                           faceEdgeMarks,
                           vindices,
                           nindices,
                           mindices,
                           tindices,
                           numVertexPerFace[index]);
        break;
      case IndexedFaceSet::TRIANGLE_FAN:
        buildTriangleFan(new_vertices,
                         new_normals,
                         frs_materials,
                         texCoords,
                         faceEdgeMarks,
                         vindices,
                         nindices,
                         mindices,
                         tindices,
                         numVertexPerFace[index]);
        break;
      case IndexedFaceSet::TRIANGLES:
        buildTriangles(new_vertices,
                       new_normals,
                       frs_materials,
                       texCoords,
                       faceEdgeMarks,
                       vindices,
                       nindices,
                       mindices,
                       tindices,
                       numVertexPerFace[index]);
        break;
    }
    vindices += numVertexPerFace[index];
    nindices += numVertexPerFace[index];
    if (mindices) {
      mindices += numVertexPerFace[index];
    }
    if (tindices) {
      tindices += numVertexPerFace[index];
    }
    faceEdgeMarks++;
  }

  delete[] new_vertices;
  delete[] new_normals;

  if (shape.GetFaceList().empty()) {  // this may happen due to degenerate triangles
    return false;
  }

#if 0
  // compute bbox
  shape.ComputeBBox();
  // compute mean edge size:
  shape.ComputeMeanEdgeSize();
#endif

  // Parse the built winged-edge shape to update post-flags
  set<Vec3f> normalsSet;
  vector<WVertex *> &wvertices = shape.getVertexList();
  for (vector<WVertex *>::iterator wv = wvertices.begin(), wvend = wvertices.end(); wv != wvend;
       ++wv) {
    if ((*wv)->isBoundary()) {
      continue;
    }
    if ((*wv)->GetEdges().empty()) {
      // This means that the WVertex has no incoming edges... (12-Sep-2011 T.K.)
      continue;
    }
    normalsSet.clear();
    WVertex::face_iterator fit = (*wv)->faces_begin();
    WVertex::face_iterator fitend = (*wv)->faces_end();
    for (; fit != fitend; ++fit) {
      WFace *face = *fit;
      normalsSet.insert(face->GetVertexNormal(*wv));
      if (normalsSet.size() != 1) {
        break;
      }
    }
    if (normalsSet.size() != 1) {
      (*wv)->setSmooth(false);
    }
  }

  // Adds the new WShape to the WingedEdge structure
  _winged_edge->addWShape(&shape);

  return true;
}

void WingedEdgeBuilder::buildWVertices(WShape &shape, const float *vertices, uint vsize)
{
  WVertex *vertex;
  for (uint i = 0; i < vsize; i += 3) {
    vertex = new WVertex(Vec3f(vertices[i], vertices[i + 1], vertices[i + 2]));
    vertex->setId(i / 3);
    shape.AddVertex(vertex);
  }
}

void WingedEdgeBuilder::buildTriangleStrip(const float * /*vertices*/,
                                           const float *normals,
                                           vector<FrsMaterial> & /*iMaterials*/,
                                           const float *texCoords,
                                           const IndexedFaceSet::FaceEdgeMark *iFaceEdgeMarks,
                                           const uint *vindices,
                                           const uint *nindices,
                                           const uint *mindices,
                                           const uint *tindices,
                                           const uint nvertices)
{
  uint nDoneVertices = 2; /* Number of vertices already treated. */
  uint nTriangle = 0;     /* Number of the triangle currently being treated. */
  // int nVertex = 0;        /* Vertex number. */

  WShape *currentShape = _current_wshape;  // the current shape being built
  vector<WVertex *> triangleVertices;
  vector<Vec3f> triangleNormals;
  vector<Vec2f> triangleTexCoords;
  vector<bool> triangleFaceEdgeMarks;

  while (nDoneVertices < nvertices) {
    // clear the vertices list:
    triangleVertices.clear();
    // Then rebuild it:
    if (0 == nTriangle % 2) {  // if nTriangle is even
      triangleVertices.push_back(currentShape->getVertexList()[vindices[nTriangle] / 3]);
      triangleVertices.push_back(currentShape->getVertexList()[vindices[nTriangle + 1] / 3]);
      triangleVertices.push_back(currentShape->getVertexList()[vindices[nTriangle + 2] / 3]);

      triangleNormals.emplace_back(normals[nindices[nTriangle]],
                                   normals[nindices[nTriangle] + 1],
                                   normals[nindices[nTriangle] + 2]);
      triangleNormals.emplace_back(normals[nindices[nTriangle + 1]],
                                   normals[nindices[nTriangle + 1] + 1],
                                   normals[nindices[nTriangle + 1] + 2]);
      triangleNormals.emplace_back(normals[nindices[nTriangle + 2]],
                                   normals[nindices[nTriangle + 2] + 1],
                                   normals[nindices[nTriangle + 2] + 2]);

      if (texCoords) {
        triangleTexCoords.emplace_back(texCoords[tindices[nTriangle]],
                                       texCoords[tindices[nTriangle] + 1]);
        triangleTexCoords.emplace_back(texCoords[tindices[nTriangle + 1]],
                                       texCoords[tindices[nTriangle + 1] + 1]);
        triangleTexCoords.emplace_back(texCoords[tindices[nTriangle + 2]],
                                       texCoords[tindices[nTriangle + 2] + 1]);
      }
    }
    else {  // if nTriangle is odd
      triangleVertices.push_back(currentShape->getVertexList()[vindices[nTriangle] / 3]);
      triangleVertices.push_back(currentShape->getVertexList()[vindices[nTriangle + 2] / 3]);
      triangleVertices.push_back(currentShape->getVertexList()[vindices[nTriangle + 1] / 3]);

      triangleNormals.emplace_back(normals[nindices[nTriangle]],
                                   normals[nindices[nTriangle] + 1],
                                   normals[nindices[nTriangle] + 2]);
      triangleNormals.emplace_back(normals[nindices[nTriangle + 2]],
                                   normals[nindices[nTriangle + 2] + 1],
                                   normals[nindices[nTriangle + 2] + 2]);
      triangleNormals.emplace_back(normals[nindices[nTriangle + 1]],
                                   normals[nindices[nTriangle + 1] + 1],
                                   normals[nindices[nTriangle + 1] + 2]);

      if (texCoords) {
        triangleTexCoords.emplace_back(texCoords[tindices[nTriangle]],
                                       texCoords[tindices[nTriangle] + 1]);
        triangleTexCoords.emplace_back(texCoords[tindices[nTriangle + 2]],
                                       texCoords[tindices[nTriangle + 2] + 1]);
        triangleTexCoords.emplace_back(texCoords[tindices[nTriangle + 1]],
                                       texCoords[tindices[nTriangle + 1] + 1]);
      }
    }
    triangleFaceEdgeMarks.push_back((iFaceEdgeMarks[nTriangle / 3] & IndexedFaceSet::FACE_MARK) !=
                                    0);
    triangleFaceEdgeMarks.push_back(
        (iFaceEdgeMarks[nTriangle / 3] & IndexedFaceSet::EDGE_MARK_V1V2) != 0);
    triangleFaceEdgeMarks.push_back(
        (iFaceEdgeMarks[nTriangle / 3] & IndexedFaceSet::EDGE_MARK_V2V3) != 0);
    triangleFaceEdgeMarks.push_back(
        (iFaceEdgeMarks[nTriangle / 3] & IndexedFaceSet::EDGE_MARK_V3V1) != 0);
    if (mindices) {
      currentShape->MakeFace(triangleVertices,
                             triangleNormals,
                             triangleTexCoords,
                             triangleFaceEdgeMarks,
                             mindices[nTriangle / 3]);
    }
    else {
      currentShape->MakeFace(
          triangleVertices, triangleNormals, triangleTexCoords, triangleFaceEdgeMarks, 0);
    }
    nDoneVertices++;  // with a strip, each triangle is one vertex more
    nTriangle++;
  }
}

void WingedEdgeBuilder::buildTriangleFan(const float * /*vertices*/,
                                         const float * /*normals*/,
                                         vector<FrsMaterial> & /*iMaterials*/,
                                         const float * /*texCoords*/,
                                         const IndexedFaceSet::FaceEdgeMark * /*iFaceEdgeMarks*/,
                                         const uint * /*vindices*/,
                                         const uint * /*nindices*/,
                                         const uint * /*mindices*/,
                                         const uint * /*tindices*/,
                                         const uint /*nvertices*/)
{
  // Nothing to be done
}

void WingedEdgeBuilder::buildTriangles(const float * /*vertices*/,
                                       const float *normals,
                                       vector<FrsMaterial> & /*iMaterials*/,
                                       const float *texCoords,
                                       const IndexedFaceSet::FaceEdgeMark *iFaceEdgeMarks,
                                       const uint *vindices,
                                       const uint *nindices,
                                       const uint *mindices,
                                       const uint *tindices,
                                       const uint nvertices)
{
  WShape *currentShape = _current_wshape;  // the current shape begin built
  vector<WVertex *> triangleVertices;
  vector<Vec3f> triangleNormals;
  vector<Vec2f> triangleTexCoords;
  vector<bool> triangleFaceEdgeMarks;

  // Each triplet of vertices is considered as an independent triangle
  for (uint i = 0; i < nvertices / 3; i++) {
    triangleVertices.push_back(currentShape->getVertexList()[vindices[3 * i] / 3]);
    triangleVertices.push_back(currentShape->getVertexList()[vindices[3 * i + 1] / 3]);
    triangleVertices.push_back(currentShape->getVertexList()[vindices[3 * i + 2] / 3]);

    triangleNormals.emplace_back(
        normals[nindices[3 * i]], normals[nindices[3 * i] + 1], normals[nindices[3 * i] + 2]);
    triangleNormals.emplace_back(normals[nindices[3 * i + 1]],
                                 normals[nindices[3 * i + 1] + 1],
                                 normals[nindices[3 * i + 1] + 2]);
    triangleNormals.emplace_back(normals[nindices[3 * i + 2]],
                                 normals[nindices[3 * i + 2] + 1],
                                 normals[nindices[3 * i + 2] + 2]);

    if (texCoords) {
      triangleTexCoords.emplace_back(texCoords[tindices[3 * i]], texCoords[tindices[3 * i] + 1]);
      triangleTexCoords.emplace_back(texCoords[tindices[3 * i + 1]],
                                     texCoords[tindices[3 * i + 1] + 1]);
      triangleTexCoords.emplace_back(texCoords[tindices[3 * i + 2]],
                                     texCoords[tindices[3 * i + 2] + 1]);
    }

    triangleFaceEdgeMarks.push_back((iFaceEdgeMarks[i] & IndexedFaceSet::FACE_MARK) != 0);
    triangleFaceEdgeMarks.push_back((iFaceEdgeMarks[i] & IndexedFaceSet::EDGE_MARK_V1V2) != 0);
    triangleFaceEdgeMarks.push_back((iFaceEdgeMarks[i] & IndexedFaceSet::EDGE_MARK_V2V3) != 0);
    triangleFaceEdgeMarks.push_back((iFaceEdgeMarks[i] & IndexedFaceSet::EDGE_MARK_V3V1) != 0);
  }
  if (mindices) {
    currentShape->MakeFace(
        triangleVertices, triangleNormals, triangleTexCoords, triangleFaceEdgeMarks, mindices[0]);
  }
  else {
    currentShape->MakeFace(
        triangleVertices, triangleNormals, triangleTexCoords, triangleFaceEdgeMarks, 0);
  }
}

void WingedEdgeBuilder::transformVertices(const float *vertices,
                                          uint vsize,
                                          const Matrix44r &transform,
                                          float *res)
{
  const float *v = vertices;
  float *pv = res;

  for (uint i = 0; i < vsize / 3; i++) {
    HVec3r hv_tmp(v[0], v[1], v[2]);
    HVec3r hv(transform * hv_tmp);
    for (uint j = 0; j < 3; j++) {
      pv[j] = hv[j] / hv[3];
    }
    v += 3;
    pv += 3;
  }
}

void WingedEdgeBuilder::transformNormals(const float *normals,
                                         uint nsize,
                                         const Matrix44r &transform,
                                         float *res)
{
  const float *n = normals;
  float *pn = res;

  for (uint i = 0; i < nsize / 3; i++) {
    Vec3r hn(n[0], n[1], n[2]);
    hn = GeomUtils::rotateVector(transform, hn);
    for (uint j = 0; j < 3; j++) {
      pn[j] = hn[j];
    }
    n += 3;
    pn += 3;
  }
}

} /* namespace Freestyle */

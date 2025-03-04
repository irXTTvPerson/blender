/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup bke
 *
 * Functions to evaluate mesh tangents.
 */

#include <limits.h>

#include "MEM_guardedalloc.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BLI_math.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"

#include "BKE_customdata.h"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_mesh_tangent.h"
#include "BKE_report.h"

#include "BLI_strict_flags.h"

#include "atomic_ops.h"
#include "mikktspace.hh"

/* -------------------------------------------------------------------- */
/** \name Mesh Tangent Calculations (Single Layer)
 * \{ */

struct BKEMeshToTangent {
  uint GetNumFaces()
  {
    return uint(num_polys);
  }

  uint GetNumVerticesOfFace(const uint face_num)
  {
    return uint(mpolys[face_num].totloop);
  }

  mikk::float3 GetPosition(const uint face_num, const uint vert_num)
  {
    const uint loop_idx = uint(mpolys[face_num].loopstart) + vert_num;
    return mikk::float3(mverts[mloops[loop_idx].v].co);
  }

  mikk::float3 GetTexCoord(const uint face_num, const uint vert_num)
  {
    const float *uv = luvs[uint(mpolys[face_num].loopstart) + vert_num].uv;
    return mikk::float3(uv[0], uv[1], 1.0f);
  }

  mikk::float3 GetNormal(const uint face_num, const uint vert_num)
  {
    return mikk::float3(lnors[uint(mpolys[face_num].loopstart) + vert_num]);
  }

  void SetTangentSpace(const uint face_num, const uint vert_num, mikk::float3 T, bool orientation)
  {
    float *p_res = tangents[uint(mpolys[face_num].loopstart) + vert_num];
    copy_v4_fl4(p_res, T.x, T.y, T.z, orientation ? 1.0f : -1.0f);
  }

  const MPoly *mpolys;     /* faces */
  const MLoop *mloops;     /* faces vertices */
  const MVert *mverts;     /* vertices */
  const MLoopUV *luvs;     /* texture coordinates */
  const float (*lnors)[3]; /* loops' normals */
  float (*tangents)[4];    /* output tangents */
  int num_polys;           /* number of polygons */
};

void BKE_mesh_calc_loop_tangent_single_ex(const MVert *mverts,
                                          const int UNUSED(numVerts),
                                          const MLoop *mloops,
                                          float (*r_looptangent)[4],
                                          const float (*loopnors)[3],
                                          const MLoopUV *loopuvs,
                                          const int UNUSED(numLoops),
                                          const MPoly *mpolys,
                                          const int numPolys,
                                          ReportList *reports)
{
  /* Compute Mikktspace's tangent normals. */
  BKEMeshToTangent mesh_to_tangent;
  mesh_to_tangent.mpolys = mpolys;
  mesh_to_tangent.mloops = mloops;
  mesh_to_tangent.mverts = mverts;
  mesh_to_tangent.luvs = loopuvs;
  mesh_to_tangent.lnors = loopnors;
  mesh_to_tangent.tangents = r_looptangent;
  mesh_to_tangent.num_polys = numPolys;

  mikk::Mikktspace<BKEMeshToTangent> mikk(mesh_to_tangent);

  /* First check we do have a tris/quads only mesh. */
  for (int i = 0; i < numPolys; i++) {
    if (mpolys[i].totloop > 4) {
      BKE_report(
          reports, RPT_ERROR, "Tangent space can only be computed for tris/quads, aborting");
      return;
    }
  }

  mikk.genTangSpace();
}

void BKE_mesh_calc_loop_tangent_single(Mesh *mesh,
                                       const char *uvmap,
                                       float (*r_looptangents)[4],
                                       ReportList *reports)
{
  const MLoopUV *loopuvs;

  /* Check we have valid texture coordinates first! */
  if (uvmap) {
    loopuvs = static_cast<MLoopUV *>(CustomData_get_layer_named(&mesh->ldata, CD_MLOOPUV, uvmap));
  }
  else {
    loopuvs = static_cast<MLoopUV *>(CustomData_get_layer(&mesh->ldata, CD_MLOOPUV));
  }
  if (!loopuvs) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Tangent space computation needs a UV Map, \"%s\" not found, aborting",
                uvmap);
    return;
  }

  const float(*loopnors)[3] = static_cast<const float(*)[3]>(
      CustomData_get_layer(&mesh->ldata, CD_NORMAL));
  if (!loopnors) {
    BKE_report(
        reports, RPT_ERROR, "Tangent space computation needs loop normals, none found, aborting");
    return;
  }

  BKE_mesh_calc_loop_tangent_single_ex(BKE_mesh_verts(mesh),
                                       mesh->totvert,
                                       BKE_mesh_loops(mesh),
                                       r_looptangents,
                                       loopnors,
                                       loopuvs,
                                       mesh->totloop,
                                       BKE_mesh_polys(mesh),
                                       mesh->totpoly,
                                       reports);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh Tangent Calculations (All Layers)
 * \{ */

/* Necessary complexity to handle looptri's as quads for correct tangents */
#define USE_LOOPTRI_DETECT_QUADS

struct SGLSLMeshToTangent {
  uint GetNumFaces()
  {
#ifdef USE_LOOPTRI_DETECT_QUADS
    return uint(num_face_as_quad_map);
#else
    return uint(numTessFaces);
#endif
  }

  uint GetNumVerticesOfFace(const uint face_num)
  {
#ifdef USE_LOOPTRI_DETECT_QUADS
    if (face_as_quad_map) {
      const MLoopTri *lt = &looptri[face_as_quad_map[face_num]];
      const MPoly *mp = &mpoly[lt->poly];
      if (mp->totloop == 4) {
        return 4;
      }
    }
    return 3;
#else
    UNUSED_VARS(pContext, face_num);
    return 3;
#endif
  }

  uint GetLoop(const uint face_num, const uint vert_num, const MLoopTri *&lt)
  {
#ifdef USE_LOOPTRI_DETECT_QUADS
    if (face_as_quad_map) {
      lt = &looptri[face_as_quad_map[face_num]];
      const MPoly *mp = &mpoly[lt->poly];
      if (mp->totloop == 4) {
        return (uint(mp->loopstart) + vert_num);
      }
      /* fall through to regular triangle */
    }
    else {
      lt = &looptri[face_num];
    }
#else
    lt = &looptri[face_num];
#endif
    return lt->tri[vert_num];
  }

  mikk::float3 GetPosition(const uint face_num, const uint vert_num)
  {
    const MLoopTri *lt;
    uint loop_index = GetLoop(face_num, vert_num, lt);
    return mikk::float3(mvert[mloop[loop_index].v].co);
  }

  mikk::float3 GetTexCoord(const uint face_num, const uint vert_num)
  {
    const MLoopTri *lt;
    uint loop_index = GetLoop(face_num, vert_num, lt);
    if (mloopuv != nullptr) {
      const float *uv = mloopuv[loop_index].uv;
      return mikk::float3(uv[0], uv[1], 1.0f);
    }
    else {
      const float *l_orco = orco[mloop[loop_index].v];
      float u, v;
      map_to_sphere(&u, &v, l_orco[0], l_orco[1], l_orco[2]);
      return mikk::float3(u, v, 1.0f);
    }
  }

  mikk::float3 GetNormal(const uint face_num, const uint vert_num)
  {
    const MLoopTri *lt;
    uint loop_index = GetLoop(face_num, vert_num, lt);
    if (precomputedLoopNormals) {
      return mikk::float3(precomputedLoopNormals[loop_index]);
    }
    else if ((mpoly[lt->poly].flag & ME_SMOOTH) == 0) { /* flat */
      if (precomputedFaceNormals) {
        return mikk::float3(precomputedFaceNormals[lt->poly]);
      }
      else {
#ifdef USE_LOOPTRI_DETECT_QUADS
        const MPoly *mp = &mpoly[lt->poly];
        float normal[3];
        if (mp->totloop == 4) {
          normal_quad_v3(normal,
                         mvert[mloop[mp->loopstart + 0].v].co,
                         mvert[mloop[mp->loopstart + 1].v].co,
                         mvert[mloop[mp->loopstart + 2].v].co,
                         mvert[mloop[mp->loopstart + 3].v].co);
        }
        else
#endif
        {
          normal_tri_v3(normal,
                        mvert[mloop[lt->tri[0]].v].co,
                        mvert[mloop[lt->tri[1]].v].co,
                        mvert[mloop[lt->tri[2]].v].co);
        }
        return mikk::float3(normal);
      }
    }
    else {
      return mikk::float3(vert_normals[mloop[loop_index].v]);
    }
  }

  void SetTangentSpace(const uint face_num, const uint vert_num, mikk::float3 T, bool orientation)
  {
    const MLoopTri *lt;
    uint loop_index = GetLoop(face_num, vert_num, lt);

    copy_v4_fl4(tangent[loop_index], T.x, T.y, T.z, orientation ? 1.0f : -1.0f);
  }

  const float (*precomputedFaceNormals)[3];
  const float (*precomputedLoopNormals)[3];
  const MLoopTri *looptri;
  const MLoopUV *mloopuv; /* texture coordinates */
  const MPoly *mpoly;     /* indices */
  const MLoop *mloop;     /* indices */
  const MVert *mvert;     /* vertex coordinates */
  const float (*vert_normals)[3];
  const float (*orco)[3];
  float (*tangent)[4]; /* destination */
  int numTessFaces;

#ifdef USE_LOOPTRI_DETECT_QUADS
  /* map from 'fake' face index to looptri,
   * quads will point to the first looptri of the quad */
  const int *face_as_quad_map;
  int num_face_as_quad_map;
#endif
};

static void DM_calc_loop_tangents_thread(TaskPool *__restrict UNUSED(pool), void *taskdata)
{
  SGLSLMeshToTangent *mesh_data = static_cast<SGLSLMeshToTangent *>(taskdata);

  mikk::Mikktspace<SGLSLMeshToTangent> mikk(*mesh_data);
  mikk.genTangSpace();
}

void BKE_mesh_add_loop_tangent_named_layer_for_uv(CustomData *uv_data,
                                                  CustomData *tan_data,
                                                  int numLoopData,
                                                  const char *layer_name)
{
  if (CustomData_get_named_layer_index(tan_data, CD_TANGENT, layer_name) == -1 &&
      CustomData_get_named_layer_index(uv_data, CD_MLOOPUV, layer_name) != -1) {
    CustomData_add_layer_named(
        tan_data, CD_TANGENT, CD_SET_DEFAULT, nullptr, numLoopData, layer_name);
  }
}

void BKE_mesh_calc_loop_tangent_step_0(const CustomData *loopData,
                                       bool calc_active_tangent,
                                       const char (*tangent_names)[MAX_NAME],
                                       int tangent_names_count,
                                       bool *rcalc_act,
                                       bool *rcalc_ren,
                                       int *ract_uv_n,
                                       int *rren_uv_n,
                                       char *ract_uv_name,
                                       char *rren_uv_name,
                                       short *rtangent_mask)
{
  /* Active uv in viewport */
  int layer_index = CustomData_get_layer_index(loopData, CD_MLOOPUV);
  *ract_uv_n = CustomData_get_active_layer(loopData, CD_MLOOPUV);
  ract_uv_name[0] = 0;
  if (*ract_uv_n != -1) {
    strcpy(ract_uv_name, loopData->layers[*ract_uv_n + layer_index].name);
  }

  /* Active tangent in render */
  *rren_uv_n = CustomData_get_render_layer(loopData, CD_MLOOPUV);
  rren_uv_name[0] = 0;
  if (*rren_uv_n != -1) {
    strcpy(rren_uv_name, loopData->layers[*rren_uv_n + layer_index].name);
  }

  /* If active tangent not in tangent_names we take it into account */
  *rcalc_act = false;
  *rcalc_ren = false;
  for (int i = 0; i < tangent_names_count; i++) {
    if (tangent_names[i][0] == 0) {
      calc_active_tangent = true;
    }
  }
  if (calc_active_tangent) {
    *rcalc_act = true;
    *rcalc_ren = true;
    for (int i = 0; i < tangent_names_count; i++) {
      if (STREQ(ract_uv_name, tangent_names[i])) {
        *rcalc_act = false;
      }
      if (STREQ(rren_uv_name, tangent_names[i])) {
        *rcalc_ren = false;
      }
    }
  }
  *rtangent_mask = 0;

  const int uv_layer_num = CustomData_number_of_layers(loopData, CD_MLOOPUV);
  for (int n = 0; n < uv_layer_num; n++) {
    const char *name = CustomData_get_layer_name(loopData, CD_MLOOPUV, n);
    bool add = false;
    for (int i = 0; i < tangent_names_count; i++) {
      if (tangent_names[i][0] && STREQ(tangent_names[i], name)) {
        add = true;
        break;
      }
    }
    if (!add && ((*rcalc_act && ract_uv_name[0] && STREQ(ract_uv_name, name)) ||
                 (*rcalc_ren && rren_uv_name[0] && STREQ(rren_uv_name, name)))) {
      add = true;
    }
    if (add) {
      *rtangent_mask |= short(1 << n);
    }
  }

  if (uv_layer_num == 0) {
    *rtangent_mask |= DM_TANGENT_MASK_ORCO;
  }
}

void BKE_mesh_calc_loop_tangent_ex(const MVert *mvert,
                                   const MPoly *mpoly,
                                   const uint mpoly_len,
                                   const MLoop *mloop,
                                   const MLoopTri *looptri,
                                   const uint looptri_len,

                                   CustomData *loopdata,
                                   bool calc_active_tangent,
                                   const char (*tangent_names)[MAX_NAME],
                                   int tangent_names_len,
                                   const float (*vert_normals)[3],
                                   const float (*poly_normals)[3],
                                   const float (*loop_normals)[3],
                                   const float (*vert_orco)[3],
                                   /* result */
                                   CustomData *loopdata_out,
                                   const uint loopdata_out_len,
                                   short *tangent_mask_curr_p)
{
  int act_uv_n = -1;
  int ren_uv_n = -1;
  bool calc_act = false;
  bool calc_ren = false;
  char act_uv_name[MAX_NAME];
  char ren_uv_name[MAX_NAME];
  short tangent_mask = 0;
  short tangent_mask_curr = *tangent_mask_curr_p;

  BKE_mesh_calc_loop_tangent_step_0(loopdata,
                                    calc_active_tangent,
                                    tangent_names,
                                    tangent_names_len,
                                    &calc_act,
                                    &calc_ren,
                                    &act_uv_n,
                                    &ren_uv_n,
                                    act_uv_name,
                                    ren_uv_name,
                                    &tangent_mask);
  if ((tangent_mask_curr | tangent_mask) != tangent_mask_curr) {
    /* Check we have all the needed layers */
    /* Allocate needed tangent layers */
    for (int i = 0; i < tangent_names_len; i++) {
      if (tangent_names[i][0]) {
        BKE_mesh_add_loop_tangent_named_layer_for_uv(
            loopdata, loopdata_out, int(loopdata_out_len), tangent_names[i]);
      }
    }
    if ((tangent_mask & DM_TANGENT_MASK_ORCO) &&
        CustomData_get_named_layer_index(loopdata, CD_TANGENT, "") == -1) {
      CustomData_add_layer_named(
          loopdata_out, CD_TANGENT, CD_SET_DEFAULT, nullptr, int(loopdata_out_len), "");
    }
    if (calc_act && act_uv_name[0]) {
      BKE_mesh_add_loop_tangent_named_layer_for_uv(
          loopdata, loopdata_out, int(loopdata_out_len), act_uv_name);
    }
    if (calc_ren && ren_uv_name[0]) {
      BKE_mesh_add_loop_tangent_named_layer_for_uv(
          loopdata, loopdata_out, int(loopdata_out_len), ren_uv_name);
    }

#ifdef USE_LOOPTRI_DETECT_QUADS
    int num_face_as_quad_map;
    int *face_as_quad_map = nullptr;

    /* map faces to quads */
    if (looptri_len != mpoly_len) {
      /* Over allocate, since we don't know how many ngon or quads we have. */

      /* map fake face index to looptri */
      face_as_quad_map = static_cast<int *>(MEM_mallocN(sizeof(int) * looptri_len, __func__));
      int k, j;
      for (k = 0, j = 0; j < int(looptri_len); k++, j++) {
        face_as_quad_map[k] = j;
        /* step over all quads */
        if (mpoly[looptri[j].poly].totloop == 4) {
          j++; /* skips the nest looptri */
        }
      }
      num_face_as_quad_map = k;
    }
    else {
      num_face_as_quad_map = int(looptri_len);
    }
#endif

    /* Calculation */
    if (looptri_len != 0) {
      TaskPool *task_pool = BLI_task_pool_create(nullptr, TASK_PRIORITY_HIGH);

      tangent_mask_curr = 0;
      /* Calculate tangent layers */
      SGLSLMeshToTangent data_array[MAX_MTFACE];
      const int tangent_layer_num = CustomData_number_of_layers(loopdata_out, CD_TANGENT);
      for (int n = 0; n < tangent_layer_num; n++) {
        int index = CustomData_get_layer_index_n(loopdata_out, CD_TANGENT, n);
        BLI_assert(n < MAX_MTFACE);
        SGLSLMeshToTangent *mesh2tangent = &data_array[n];
        mesh2tangent->numTessFaces = int(looptri_len);
#ifdef USE_LOOPTRI_DETECT_QUADS
        mesh2tangent->face_as_quad_map = face_as_quad_map;
        mesh2tangent->num_face_as_quad_map = num_face_as_quad_map;
#endif
        mesh2tangent->mvert = mvert;
        mesh2tangent->vert_normals = vert_normals;
        mesh2tangent->mpoly = mpoly;
        mesh2tangent->mloop = mloop;
        mesh2tangent->looptri = looptri;
        /* NOTE: we assume we do have tessellated loop normals at this point
         * (in case it is object-enabled), have to check this is valid. */
        mesh2tangent->precomputedLoopNormals = loop_normals;
        mesh2tangent->precomputedFaceNormals = poly_normals;

        mesh2tangent->orco = nullptr;
        mesh2tangent->mloopuv = static_cast<const MLoopUV *>(
            CustomData_get_layer_named(loopdata, CD_MLOOPUV, loopdata_out->layers[index].name));

        /* Fill the resulting tangent_mask */
        if (!mesh2tangent->mloopuv) {
          mesh2tangent->orco = vert_orco;
          if (!mesh2tangent->orco) {
            continue;
          }

          tangent_mask_curr |= DM_TANGENT_MASK_ORCO;
        }
        else {
          int uv_ind = CustomData_get_named_layer_index(
              loopdata, CD_MLOOPUV, loopdata_out->layers[index].name);
          int uv_start = CustomData_get_layer_index(loopdata, CD_MLOOPUV);
          BLI_assert(uv_ind != -1 && uv_start != -1);
          BLI_assert(uv_ind - uv_start < MAX_MTFACE);
          tangent_mask_curr |= short(1 << (uv_ind - uv_start));
        }

        mesh2tangent->tangent = static_cast<float(*)[4]>(loopdata_out->layers[index].data);
        BLI_task_pool_push(task_pool, DM_calc_loop_tangents_thread, mesh2tangent, false, nullptr);
      }

      BLI_assert(tangent_mask_curr == tangent_mask);
      BLI_task_pool_work_and_wait(task_pool);
      BLI_task_pool_free(task_pool);
    }
    else {
      tangent_mask_curr = tangent_mask;
    }
#ifdef USE_LOOPTRI_DETECT_QUADS
    if (face_as_quad_map) {
      MEM_freeN(face_as_quad_map);
    }
#  undef USE_LOOPTRI_DETECT_QUADS

#endif

    *tangent_mask_curr_p = tangent_mask_curr;

    /* Update active layer index */
    int act_uv_index = (act_uv_n != -1) ?
                           CustomData_get_layer_index_n(loopdata, CD_MLOOPUV, act_uv_n) :
                           -1;
    if (act_uv_index != -1) {
      int tan_index = CustomData_get_named_layer_index(
          loopdata, CD_TANGENT, loopdata->layers[act_uv_index].name);
      CustomData_set_layer_active_index(loopdata, CD_TANGENT, tan_index);
    } /* else tangent has been built from orco */

    /* Update render layer index */
    int ren_uv_index = (ren_uv_n != -1) ?
                           CustomData_get_layer_index_n(loopdata, CD_MLOOPUV, ren_uv_n) :
                           -1;
    if (ren_uv_index != -1) {
      int tan_index = CustomData_get_named_layer_index(
          loopdata, CD_TANGENT, loopdata->layers[ren_uv_index].name);
      CustomData_set_layer_render_index(loopdata, CD_TANGENT, tan_index);
    } /* else tangent has been built from orco */
  }
}

void BKE_mesh_calc_loop_tangents(Mesh *me_eval,
                                 bool calc_active_tangent,
                                 const char (*tangent_names)[MAX_NAME],
                                 int tangent_names_len)
{
  BKE_mesh_runtime_looptri_ensure(me_eval);

  /* TODO(@campbellbarton): store in Mesh.runtime to avoid recalculation. */
  short tangent_mask = 0;
  BKE_mesh_calc_loop_tangent_ex(
      BKE_mesh_verts(me_eval),
      BKE_mesh_polys(me_eval),
      uint(me_eval->totpoly),
      BKE_mesh_loops(me_eval),
      me_eval->runtime.looptris.array,
      uint(me_eval->runtime.looptris.len),
      &me_eval->ldata,
      calc_active_tangent,
      tangent_names,
      tangent_names_len,
      BKE_mesh_vertex_normals_ensure(me_eval),
      BKE_mesh_poly_normals_ensure(me_eval),
      static_cast<const float(*)[3]>(CustomData_get_layer(&me_eval->ldata, CD_NORMAL)),
      /* may be nullptr */
      static_cast<const float(*)[3]>(CustomData_get_layer(&me_eval->vdata, CD_ORCO)),
      /* result */
      &me_eval->ldata,
      uint(me_eval->totloop),
      &tangent_mask);
}

/** \} */

/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include <limits.h>
#include <math.h>
#include <memory.h>
#include <stddef.h>

#include "MEM_guardedalloc.h"

#include "DNA_anim_types.h"
#include "DNA_camera_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_movieclip_types.h"
#include "DNA_object_types.h" /* SELECT */
#include "DNA_scene_types.h"

#include "BLI_bitmap_draw_2d.h"
#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_math_base.h"
#include "BLI_string.h"
#include "BLI_string_utils.h"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_fcurve.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_movieclip.h"
#include "BKE_object.h"
#include "BKE_scene.h"
#include "BKE_tracking.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "libmv-capi.h"
#include "tracking_private.h"

typedef struct MovieDistortion {
  struct libmv_CameraIntrinsics *intrinsics;
  /* Parameters needed for coordinates normalization. */
  float principal[2];
  float pixel_aspect;
  float focal;
} MovieDistortion;

static struct {
  ListBase tracks;
} tracking_clipboard;

/* --------------------------------------------------------------------
 * Common functions.
 */

/* Free the whole list of tracks, list's head and tail are set to NULL. */
static void tracking_tracks_free(ListBase *tracks)
{
  LISTBASE_FOREACH (MovieTrackingTrack *, track, tracks) {
    BKE_tracking_track_free(track);
  }

  BLI_freelistN(tracks);
}

/* Free the whole list of plane tracks, list's head and tail are set to NULL. */
static void tracking_plane_tracks_free(ListBase *plane_tracks)
{
  LISTBASE_FOREACH (MovieTrackingPlaneTrack *, plane_track, plane_tracks) {
    BKE_tracking_plane_track_free(plane_track);
  }

  BLI_freelistN(plane_tracks);
}

/* Free reconstruction structures, only frees contents of a structure,
 * (if structure is allocated in heap, it shall be handled outside).
 *
 * All the pointers inside structure becomes invalid after this call.
 */
static void tracking_reconstruction_free(MovieTrackingReconstruction *reconstruction)
{
  if (reconstruction->cameras) {
    MEM_freeN(reconstruction->cameras);
  }
}

/* Free memory used by tracking object, only frees contents of the structure,
 * (if structure is allocated in heap, it shall be handled outside).
 *
 * All the pointers inside structure becomes invalid after this call.
 */
static void tracking_object_free(MovieTrackingObject *object)
{
  tracking_tracks_free(&object->tracks);
  tracking_plane_tracks_free(&object->plane_tracks);
  tracking_reconstruction_free(&object->reconstruction);
}

/* Free list of tracking objects, list's head and tail is set to NULL. */
static void tracking_objects_free(ListBase *objects)
{
  /* Free objects contents. */
  LISTBASE_FOREACH (MovieTrackingObject *, object, objects) {
    tracking_object_free(object);
  }

  /* Free objects themselves. */
  BLI_freelistN(objects);
}

/* Free memory used by a dopesheet, only frees dopesheet contents.
 * leaving dopesheet crystal clean for further usage.
 */
static void tracking_dopesheet_free(MovieTrackingDopesheet *dopesheet)
{
  MovieTrackingDopesheetChannel *channel;

  /* Free channel's segments. */
  channel = dopesheet->channels.first;
  while (channel) {
    if (channel->segments) {
      MEM_freeN(channel->segments);
    }

    channel = channel->next;
  }

  /* Free lists themselves. */
  BLI_freelistN(&dopesheet->channels);
  BLI_freelistN(&dopesheet->coverage_segments);

  /* Ensure lists are clean. */
  BLI_listbase_clear(&dopesheet->channels);
  BLI_listbase_clear(&dopesheet->coverage_segments);
  dopesheet->tot_channel = 0;
}

void BKE_tracking_free(MovieTracking *tracking)
{
  tracking_tracks_free(&tracking->tracks);
  tracking_plane_tracks_free(&tracking->plane_tracks);
  tracking_reconstruction_free(&tracking->reconstruction);
  tracking_objects_free(&tracking->objects);

  if (tracking->camera.intrinsics) {
    BKE_tracking_distortion_free(tracking->camera.intrinsics);
  }

  tracking_dopesheet_free(&tracking->dopesheet);
}

/* Copy the whole list of tracks. */
static void tracking_tracks_copy(ListBase *tracks_dst,
                                 const ListBase *tracks_src,
                                 GHash *tracks_mapping,
                                 const int flag)
{
  BLI_listbase_clear(tracks_dst);
  BLI_ghash_clear(tracks_mapping, NULL, NULL);

  LISTBASE_FOREACH (MovieTrackingTrack *, track_src, tracks_src) {
    MovieTrackingTrack *track_dst = MEM_dupallocN(track_src);
    if (track_src->markers) {
      track_dst->markers = MEM_dupallocN(track_src->markers);
    }
    if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
      id_us_plus(&track_dst->gpd->id);
    }
    BLI_addtail(tracks_dst, track_dst);
    BLI_ghash_insert(tracks_mapping, track_src, track_dst);
  }
}

/* Copy the whole list of plane tracks
 * (need whole MovieTracking structures due to embedded pointers to tracks).
 * WARNING: implies tracking_[dst/src] and their tracks have already been copied. */
static void tracking_plane_tracks_copy(ListBase *plane_tracks_list_dst,
                                       const ListBase *plane_tracks_list_src,
                                       GHash *tracks_mapping,
                                       const int flag)
{
  BLI_listbase_clear(plane_tracks_list_dst);

  LISTBASE_FOREACH (MovieTrackingPlaneTrack *, plane_track_src, plane_tracks_list_src) {
    MovieTrackingPlaneTrack *plane_track_dst = MEM_dupallocN(plane_track_src);
    if (plane_track_src->markers) {
      plane_track_dst->markers = MEM_dupallocN(plane_track_src->markers);
    }
    plane_track_dst->point_tracks = MEM_mallocN(
        sizeof(*plane_track_dst->point_tracks) * plane_track_dst->point_tracksnr, __func__);
    for (int i = 0; i < plane_track_dst->point_tracksnr; i++) {
      plane_track_dst->point_tracks[i] = BLI_ghash_lookup(tracks_mapping,
                                                          plane_track_src->point_tracks[i]);
    }
    if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
      id_us_plus(&plane_track_dst->image->id);
    }
    BLI_addtail(plane_tracks_list_dst, plane_track_dst);
  }
}

/* Copy reconstruction structure. */
static void tracking_reconstruction_copy(MovieTrackingReconstruction *reconstruction_dst,
                                         const MovieTrackingReconstruction *reconstruction_src,
                                         const int UNUSED(flag))
{
  *reconstruction_dst = *reconstruction_src;
  if (reconstruction_src->cameras) {
    reconstruction_dst->cameras = MEM_dupallocN(reconstruction_src->cameras);
  }
}

/* Copy stabilization structure. */
static void tracking_stabilization_copy(MovieTrackingStabilization *stabilization_dst,
                                        const MovieTrackingStabilization *stabilization_src,
                                        const int UNUSED(flag))
{
  *stabilization_dst = *stabilization_src;
}

/* Copy tracking object. */
static void tracking_object_copy(MovieTrackingObject *object_dst,
                                 const MovieTrackingObject *object_src,
                                 GHash *tracks_mapping,
                                 const int flag)
{
  *object_dst = *object_src;
  tracking_tracks_copy(&object_dst->tracks, &object_src->tracks, tracks_mapping, flag);
  tracking_plane_tracks_copy(
      &object_dst->plane_tracks, &object_src->plane_tracks, tracks_mapping, flag);
  tracking_reconstruction_copy(&object_dst->reconstruction, &object_src->reconstruction, flag);
}

/* Copy list of tracking objects. */
static void tracking_objects_copy(ListBase *objects_dst,
                                  const ListBase *objects_src,
                                  GHash *tracks_mapping,
                                  const int flag)
{
  BLI_listbase_clear(objects_dst);

  LISTBASE_FOREACH (MovieTrackingObject *, object_src, objects_src) {
    MovieTrackingObject *object_dst = MEM_mallocN(sizeof(*object_dst), __func__);
    tracking_object_copy(object_dst, object_src, tracks_mapping, flag);
    BLI_addtail(objects_dst, object_dst);
  }
}

void BKE_tracking_copy(MovieTracking *tracking_dst,
                       const MovieTracking *tracking_src,
                       const int flag)
{
  GHash *tracks_mapping = BLI_ghash_ptr_new(__func__);

  *tracking_dst = *tracking_src;

  tracking_tracks_copy(&tracking_dst->tracks, &tracking_src->tracks, tracks_mapping, flag);
  tracking_plane_tracks_copy(
      &tracking_dst->plane_tracks, &tracking_src->plane_tracks, tracks_mapping, flag);
  tracking_reconstruction_copy(&tracking_dst->reconstruction, &tracking_src->reconstruction, flag);
  tracking_stabilization_copy(&tracking_dst->stabilization, &tracking_src->stabilization, flag);
  if (tracking_src->act_track) {
    tracking_dst->act_track = BLI_ghash_lookup(tracks_mapping, tracking_src->act_track);
  }
  if (tracking_src->act_plane_track) {
    MovieTrackingPlaneTrack *plane_track_src, *plane_track_dst;
    for (plane_track_src = tracking_src->plane_tracks.first,
        plane_track_dst = tracking_dst->plane_tracks.first;
         !ELEM(NULL, plane_track_src, plane_track_dst);
         plane_track_src = plane_track_src->next, plane_track_dst = plane_track_dst->next) {
      if (plane_track_src == tracking_src->act_plane_track) {
        tracking_dst->act_plane_track = plane_track_dst;
        break;
      }
    }
  }

  /* Warning! Will override tracks_mapping. */
  tracking_objects_copy(&tracking_dst->objects, &tracking_src->objects, tracks_mapping, flag);

  /* Those remaining are runtime data, they will be reconstructed as needed,
   * do not bother copying them. */
  tracking_dst->dopesheet.ok = false;
  BLI_listbase_clear(&tracking_dst->dopesheet.channels);
  BLI_listbase_clear(&tracking_dst->dopesheet.coverage_segments);

  tracking_dst->camera.intrinsics = NULL;
  tracking_dst->stats = NULL;

  BLI_ghash_free(tracks_mapping, NULL, NULL);
}

void BKE_tracking_settings_init(MovieTracking *tracking)
{
  tracking->camera.sensor_width = 35.0f;
  tracking->camera.pixel_aspect = 1.0f;
  tracking->camera.units = CAMERA_UNITS_MM;

  tracking->settings.default_motion_model = TRACK_MOTION_MODEL_TRANSLATION;
  tracking->settings.default_minimum_correlation = 0.75;
  tracking->settings.default_pattern_size = 21;
  tracking->settings.default_search_size = 71;
  tracking->settings.default_algorithm_flag |= TRACK_ALGORITHM_FLAG_USE_BRUTE;
  tracking->settings.default_weight = 1.0f;
  tracking->settings.dist = 1;
  tracking->settings.object_distance = 1;
  tracking->settings.refine_camera_intrinsics = REFINE_NO_INTRINSICS;

  tracking->stabilization.scaleinf = 1.0f;
  tracking->stabilization.anchor_frame = 1;
  zero_v2(tracking->stabilization.target_pos);
  tracking->stabilization.target_rot = 0.0f;
  tracking->stabilization.scale = 1.0f;

  tracking->stabilization.act_track = 0;
  tracking->stabilization.act_rot_track = 0;
  tracking->stabilization.tot_track = 0;
  tracking->stabilization.tot_rot_track = 0;

  tracking->stabilization.scaleinf = 1.0f;
  tracking->stabilization.locinf = 1.0f;
  tracking->stabilization.rotinf = 1.0f;
  tracking->stabilization.maxscale = 2.0f;
  tracking->stabilization.filter = TRACKING_FILTER_BILINEAR;
  tracking->stabilization.flag |= TRACKING_SHOW_STAB_TRACKS;

  /* Descending order of average error: tracks with the highest error are on top. */
  tracking->dopesheet.sort_method = TRACKING_DOPE_SORT_AVERAGE_ERROR;
  tracking->dopesheet.flag |= TRACKING_DOPE_SORT_INVERSE;

  BKE_tracking_object_add(tracking, DATA_("Camera"));
}

ListBase *BKE_tracking_get_active_tracks(MovieTracking *tracking)
{
  MovieTrackingObject *object = BKE_tracking_object_get_active(tracking);

  if (object && (object->flag & TRACKING_OBJECT_CAMERA) == 0) {
    return &object->tracks;
  }

  return &tracking->tracks;
}

ListBase *BKE_tracking_get_active_plane_tracks(MovieTracking *tracking)
{
  MovieTrackingObject *object = BKE_tracking_object_get_active(tracking);

  if (object && (object->flag & TRACKING_OBJECT_CAMERA) == 0) {
    return &object->plane_tracks;
  }

  return &tracking->plane_tracks;
}

MovieTrackingReconstruction *BKE_tracking_get_active_reconstruction(MovieTracking *tracking)
{
  MovieTrackingObject *object = BKE_tracking_object_get_active(tracking);

  return BKE_tracking_object_get_reconstruction(tracking, object);
}

void BKE_tracking_get_camera_object_matrix(Object *camera_object, float mat[4][4])
{
  BLI_assert(camera_object != NULL);
  /* NOTE: Construct matrix from scratch rather than using obmat because the camera object here
   * will have camera solver constraint taken into account. But here we do not want or need it:
   * object is solved in camera space (as in, camera is stationary and object is moving).
   *
   * This will include animation applied on the camera, but not possible camera rig. This isn't
   * an issue in practice due to the way how VFX is constructed.
   *
   * If we ever need to support crazy setups like that one possible solution would be to use
   * final camera matrix and multiple it by an inverse of solved camera matrix at the current
   * frame. */
  BKE_object_where_is_calc_mat4(camera_object, mat);
}

void BKE_tracking_get_projection_matrix(MovieTracking *tracking,
                                        MovieTrackingObject *object,
                                        int framenr,
                                        int winx,
                                        int winy,
                                        float mat[4][4])
{
  MovieReconstructedCamera *camera;
  float lens = tracking->camera.focal * tracking->camera.sensor_width / (float)winx;
  float viewfac, pixsize, left, right, bottom, top, clipsta, clipend;
  float winmat[4][4];
  float ycor = 1.0f / tracking->camera.pixel_aspect;
  float shiftx, shifty, winside = (float)min_ii(winx, winy);

  BKE_tracking_camera_shift_get(tracking, winx, winy, &shiftx, &shifty);

  clipsta = 0.1f;
  clipend = 1000.0f;

  if (winx >= winy) {
    viewfac = (lens * winx) / tracking->camera.sensor_width;
  }
  else {
    viewfac = (ycor * lens * winy) / tracking->camera.sensor_width;
  }

  pixsize = clipsta / viewfac;

  left = -0.5f * (float)winx + shiftx * winside;
  bottom = -0.5f * (ycor) * (float)winy + shifty * winside;
  right = 0.5f * (float)winx + shiftx * winside;
  top = 0.5f * (ycor) * (float)winy + shifty * winside;

  left *= pixsize;
  right *= pixsize;
  bottom *= pixsize;
  top *= pixsize;

  perspective_m4(winmat, left, right, bottom, top, clipsta, clipend);

  camera = BKE_tracking_camera_get_reconstructed(tracking, object, framenr);

  if (camera) {
    float imat[4][4];

    invert_m4_m4(imat, camera->mat);
    mul_m4_m4m4(mat, winmat, imat);
  }
  else {
    copy_m4_m4(mat, winmat);
  }
}

/* --------------------------------------------------------------------
 * Clipboard.
 */

void BKE_tracking_clipboard_free(void)
{
  MovieTrackingTrack *track = tracking_clipboard.tracks.first, *next_track;

  while (track) {
    next_track = track->next;

    BKE_tracking_track_free(track);
    MEM_freeN(track);

    track = next_track;
  }

  BLI_listbase_clear(&tracking_clipboard.tracks);
}

void BKE_tracking_clipboard_copy_tracks(MovieTracking *tracking, MovieTrackingObject *object)
{
  ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);
  MovieTrackingTrack *track = tracksbase->first;

  /* First drop all tracks from current clipboard. */
  BKE_tracking_clipboard_free();

  /* Then copy all selected visible tracks to it. */
  while (track) {
    if (TRACK_SELECTED(track) && (track->flag & TRACK_HIDDEN) == 0) {
      MovieTrackingTrack *new_track = BKE_tracking_track_duplicate(track);

      BLI_addtail(&tracking_clipboard.tracks, new_track);
    }

    track = track->next;
  }
}

bool BKE_tracking_clipboard_has_tracks(void)
{
  return (BLI_listbase_is_empty(&tracking_clipboard.tracks) == false);
}

void BKE_tracking_clipboard_paste_tracks(MovieTracking *tracking, MovieTrackingObject *object)
{
  ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);
  MovieTrackingTrack *track = tracking_clipboard.tracks.first;

  while (track) {
    MovieTrackingTrack *new_track = BKE_tracking_track_duplicate(track);
    if (track->prev == NULL) {
      tracking->act_track = new_track;
    }

    BLI_addtail(tracksbase, new_track);
    BKE_tracking_track_unique_name(tracksbase, new_track);

    track = track->next;
  }
}

/* --------------------------------------------------------------------
 * Tracks.
 */

MovieTrackingTrack *BKE_tracking_track_add_empty(MovieTracking *tracking, ListBase *tracks_list)
{
  const MovieTrackingSettings *settings = &tracking->settings;

  MovieTrackingTrack *track = MEM_callocN(sizeof(MovieTrackingTrack), "add_marker_exec track");
  strcpy(track->name, "Track");

  /* Fill track's settings from default settings. */
  track->motion_model = settings->default_motion_model;
  track->minimum_correlation = settings->default_minimum_correlation;
  track->margin = settings->default_margin;
  track->pattern_match = settings->default_pattern_match;
  track->frames_limit = settings->default_frames_limit;
  track->flag = settings->default_flag;
  track->algorithm_flag = settings->default_algorithm_flag;
  track->weight = settings->default_weight;
  track->weight_stab = settings->default_weight;

  BLI_addtail(tracks_list, track);
  BKE_tracking_track_unique_name(tracks_list, track);

  return track;
}

MovieTrackingTrack *BKE_tracking_track_add(MovieTracking *tracking,
                                           ListBase *tracksbase,
                                           float x,
                                           float y,
                                           int framenr,
                                           int width,
                                           int height)
{
  const MovieTrackingSettings *settings = &tracking->settings;

  MovieTrackingTrack *track = BKE_tracking_track_add_empty(tracking, tracksbase);
  MovieTrackingMarker marker;

  const float half_pattern_px = settings->default_pattern_size / 2.0f;
  const float half_search_px = settings->default_search_size / 2.0f;

  const float pattern_size[2] = {half_pattern_px / width, half_pattern_px / height};
  const float search_size[2] = {half_search_px / width, half_search_px / height};

  memset(&marker, 0, sizeof(marker));
  marker.pos[0] = x;
  marker.pos[1] = y;
  marker.framenr = framenr;

  marker.pattern_corners[0][0] = -pattern_size[0];
  marker.pattern_corners[0][1] = -pattern_size[1];

  marker.pattern_corners[1][0] = pattern_size[0];
  marker.pattern_corners[1][1] = -pattern_size[1];

  negate_v2_v2(marker.pattern_corners[2], marker.pattern_corners[0]);
  negate_v2_v2(marker.pattern_corners[3], marker.pattern_corners[1]);

  copy_v2_v2(marker.search_max, search_size);
  negate_v2_v2(marker.search_min, search_size);

  BKE_tracking_marker_insert(track, &marker);

  return track;
}

MovieTrackingTrack *BKE_tracking_track_duplicate(MovieTrackingTrack *track)
{
  MovieTrackingTrack *new_track;

  new_track = MEM_callocN(sizeof(MovieTrackingTrack), "tracking_track_duplicate new_track");

  *new_track = *track;
  new_track->next = new_track->prev = NULL;

  new_track->markers = MEM_dupallocN(new_track->markers);

  /* Prevent duplicate from being used for 2D stabilization.
   * If necessary, it shall be added explicitly.
   */
  new_track->flag &= ~TRACK_USE_2D_STAB;
  new_track->flag &= ~TRACK_USE_2D_STAB_ROT;

  return new_track;
}

void BKE_tracking_track_unique_name(ListBase *tracksbase, MovieTrackingTrack *track)
{
  BLI_uniquename(tracksbase,
                 track,
                 CTX_DATA_(BLT_I18NCONTEXT_ID_MOVIECLIP, "Track"),
                 '.',
                 offsetof(MovieTrackingTrack, name),
                 sizeof(track->name));
}

void BKE_tracking_track_free(MovieTrackingTrack *track)
{
  if (track->markers) {
    MEM_freeN(track->markers);
  }
}

void BKE_tracking_track_first_last_frame_get(const MovieTrackingTrack *track,
                                             int *r_first_frame,
                                             int *r_last_frame)
{
  BLI_assert(track->markersnr > 0);
  const int last_marker_index = track->markersnr - 1;
  *r_first_frame = track->markers[0].framenr;
  *r_last_frame = track->markers[last_marker_index].framenr;
}

void BKE_tracking_tracks_first_last_frame_minmax(/*const*/ MovieTrackingTrack **tracks,
                                                 const int num_tracks,
                                                 int *r_first_frame,
                                                 int *r_last_frame)
{
  *r_first_frame = INT_MAX;
  *r_last_frame = INT_MIN;
  for (int i = 0; i < num_tracks; ++i) {
    const struct MovieTrackingTrack *track = tracks[i];
    int track_first_frame, track_last_frame;
    BKE_tracking_track_first_last_frame_get(track, &track_first_frame, &track_last_frame);
    *r_first_frame = min_ii(*r_first_frame, track_first_frame);
    *r_last_frame = max_ii(*r_last_frame, track_last_frame);
  }
}

int BKE_tracking_count_selected_tracks_in_list(const ListBase *tracks_list)
{
  int num_selected_tracks = 0;
  LISTBASE_FOREACH (const MovieTrackingTrack *, track, tracks_list) {
    if (TRACK_SELECTED(track)) {
      ++num_selected_tracks;
    }
  }
  return num_selected_tracks;
}

int BKE_tracking_count_selected_tracks_in_active_object(/*const*/ MovieTracking *tracking)
{
  ListBase *tracks_list = BKE_tracking_get_active_tracks(tracking);
  return BKE_tracking_count_selected_tracks_in_list(tracks_list);
}

MovieTrackingTrack **BKE_tracking_selected_tracks_in_active_object(MovieTracking *tracking,
                                                                   int *r_num_tracks)
{
  *r_num_tracks = 0;

  ListBase *tracks_list = BKE_tracking_get_active_tracks(tracking);
  if (tracks_list == NULL) {
    return NULL;
  }

  /* Initialize input. */
  const int num_selected_tracks = BKE_tracking_count_selected_tracks_in_active_object(tracking);
  if (num_selected_tracks == 0) {
    return NULL;
  }

  MovieTrackingTrack **source_tracks = MEM_malloc_arrayN(
      num_selected_tracks, sizeof(MovieTrackingTrack *), "selected tracks array");
  int source_track_index = 0;
  LISTBASE_FOREACH (MovieTrackingTrack *, track, tracks_list) {
    if (!TRACK_SELECTED(track)) {
      continue;
    }
    source_tracks[source_track_index] = track;
    ++source_track_index;
  }

  *r_num_tracks = num_selected_tracks;

  return source_tracks;
}

void BKE_tracking_track_flag_set(MovieTrackingTrack *track, int area, int flag)
{
  if (area == TRACK_AREA_NONE) {
    return;
  }

  if (area & TRACK_AREA_POINT) {
    track->flag |= flag;
  }
  if (area & TRACK_AREA_PAT) {
    track->pat_flag |= flag;
  }
  if (area & TRACK_AREA_SEARCH) {
    track->search_flag |= flag;
  }
}

void BKE_tracking_track_flag_clear(MovieTrackingTrack *track, int area, int flag)
{
  if (area == TRACK_AREA_NONE) {
    return;
  }

  if (area & TRACK_AREA_POINT) {
    track->flag &= ~flag;
  }
  if (area & TRACK_AREA_PAT) {
    track->pat_flag &= ~flag;
  }
  if (area & TRACK_AREA_SEARCH) {
    track->search_flag &= ~flag;
  }
}

bool BKE_tracking_track_has_marker_at_frame(MovieTrackingTrack *track, int framenr)
{
  return BKE_tracking_marker_get_exact(track, framenr) != NULL;
}

bool BKE_tracking_track_has_enabled_marker_at_frame(MovieTrackingTrack *track, int framenr)
{
  MovieTrackingMarker *marker = BKE_tracking_marker_get_exact(track, framenr);

  return marker && (marker->flag & MARKER_DISABLED) == 0;
}

static void path_clear_remained(MovieTrackingTrack *track, const int ref_frame)
{
  for (int a = 1; a < track->markersnr; a++) {
    if (track->markers[a].framenr > ref_frame) {
      track->markersnr = a;
      track->markers = MEM_reallocN(track->markers,
                                    sizeof(MovieTrackingMarker) * track->markersnr);

      break;
    }
  }

  if (track->markersnr) {
    tracking_marker_insert_disabled(track, &track->markers[track->markersnr - 1], false, true);
  }
}

static void path_clear_up_to(MovieTrackingTrack *track, const int ref_frame)
{
  for (int a = track->markersnr - 1; a >= 0; a--) {
    if (track->markers[a].framenr <= ref_frame) {
      memmove(track->markers,
              track->markers + a,
              (track->markersnr - a) * sizeof(MovieTrackingMarker));

      track->markersnr = track->markersnr - a;
      track->markers = MEM_reallocN(track->markers,
                                    sizeof(MovieTrackingMarker) * track->markersnr);

      break;
    }
  }

  if (track->markersnr) {
    tracking_marker_insert_disabled(track, &track->markers[0], true, true);
  }
}

static void path_clear_all(MovieTrackingTrack *track, const int ref_frame)
{
  MovieTrackingMarker *marker, marker_new;

  marker = BKE_tracking_marker_get(track, ref_frame);
  marker_new = *marker;

  MEM_freeN(track->markers);
  track->markers = NULL;
  track->markersnr = 0;

  BKE_tracking_marker_insert(track, &marker_new);

  tracking_marker_insert_disabled(track, &marker_new, true, true);
  tracking_marker_insert_disabled(track, &marker_new, false, true);
}

void BKE_tracking_track_path_clear(MovieTrackingTrack *track,
                                   const int ref_frame,
                                   const eTrackClearAction action)
{
  switch (action) {
    case TRACK_CLEAR_REMAINED:
      path_clear_remained(track, ref_frame);
      break;
    case TRACK_CLEAR_UPTO:
      path_clear_up_to(track, ref_frame);
      break;
    case TRACK_CLEAR_ALL:
      path_clear_all(track, ref_frame);
      break;
  };
}

void BKE_tracking_tracks_join(MovieTracking *tracking,
                              MovieTrackingTrack *dst_track,
                              MovieTrackingTrack *src_track)
{
  int i = 0, a = 0, b = 0, tot;
  MovieTrackingMarker *markers;

  tot = dst_track->markersnr + src_track->markersnr;
  markers = MEM_callocN(tot * sizeof(MovieTrackingMarker), "tmp tracking joined tracks");

  while (a < src_track->markersnr || b < dst_track->markersnr) {
    if (b >= dst_track->markersnr) {
      markers[i] = src_track->markers[a++];
    }
    else if (a >= src_track->markersnr) {
      markers[i] = dst_track->markers[b++];
    }
    else if (src_track->markers[a].framenr < dst_track->markers[b].framenr) {
      markers[i] = src_track->markers[a++];
    }
    else if (src_track->markers[a].framenr > dst_track->markers[b].framenr) {
      markers[i] = dst_track->markers[b++];
    }
    else {
      if ((src_track->markers[a].flag & MARKER_DISABLED) == 0) {
        if ((dst_track->markers[b].flag & MARKER_DISABLED) == 0) {
          /* both tracks are enabled on this frame, so find the whole segment
           * on which tracks are intersecting and blend tracks using linear
           * interpolation to prevent jumps
           */

          MovieTrackingMarker *marker_a, *marker_b;
          int start_a = a, start_b = b, len = 0, frame = src_track->markers[a].framenr;
          int inverse = 0;

          inverse = (b == 0) || (dst_track->markers[b - 1].flag & MARKER_DISABLED) ||
                    (dst_track->markers[b - 1].framenr != frame - 1);

          /* find length of intersection */
          while (a < src_track->markersnr && b < dst_track->markersnr) {
            marker_a = &src_track->markers[a];
            marker_b = &dst_track->markers[b];

            if (marker_a->flag & MARKER_DISABLED || marker_b->flag & MARKER_DISABLED) {
              break;
            }

            if (marker_a->framenr != frame || marker_b->framenr != frame) {
              break;
            }

            frame++;
            len++;
            a++;
            b++;
          }

          a = start_a;
          b = start_b;

          /* linear interpolation for intersecting frames */
          for (int j = 0; j < len; j++) {
            float fac = 0.5f;

            if (len > 1) {
              fac = 1.0f / (len - 1) * j;
            }

            if (inverse) {
              fac = 1.0f - fac;
            }

            marker_a = &src_track->markers[a];
            marker_b = &dst_track->markers[b];

            markers[i] = dst_track->markers[b];
            interp_v2_v2v2(markers[i].pos, marker_b->pos, marker_a->pos, fac);
            a++;
            b++;
            i++;
          }

          /* this values will be incremented at the end of the loop cycle */
          a--;
          b--;
          i--;
        }
        else {
          markers[i] = src_track->markers[a];
        }
      }
      else {
        markers[i] = dst_track->markers[b];
      }

      a++;
      b++;
    }

    i++;
  }

  MEM_freeN(dst_track->markers);

  dst_track->markers = MEM_mallocN(i * sizeof(MovieTrackingMarker), "tracking joined tracks");
  memcpy(dst_track->markers, markers, i * sizeof(MovieTrackingMarker));

  dst_track->markersnr = i;

  MEM_freeN(markers);

  BKE_tracking_dopesheet_tag_update(tracking);
}

static void accumulate_marker(MovieTrackingMarker *dst_marker,
                              const MovieTrackingMarker *src_marker)
{
  BLI_assert(dst_marker->framenr == src_marker->framenr);

  if (src_marker->flag & MARKER_DISABLED) {
    return;
  }

  add_v2_v2(dst_marker->pos, src_marker->pos);
  for (int corner = 0; corner < 4; ++corner) {
    add_v2_v2(dst_marker->pattern_corners[corner], src_marker->pattern_corners[corner]);
  }
  add_v2_v2(dst_marker->search_min, src_marker->search_min);
  add_v2_v2(dst_marker->search_max, src_marker->search_max);

  BLI_assert(is_finite_v2(src_marker->search_min));
  BLI_assert(is_finite_v2(src_marker->search_max));

  dst_marker->flag &= ~MARKER_DISABLED;
  if ((src_marker->flag & MARKER_TRACKED) == 0) {
    dst_marker->flag &= ~MARKER_TRACKED;
  }
}

static void multiply_marker(MovieTrackingMarker *marker, const float multiplier)
{
  mul_v2_fl(marker->pos, multiplier);
  for (int corner = 0; corner < 4; ++corner) {
    mul_v2_fl(marker->pattern_corners[corner], multiplier);
  }
  mul_v2_fl(marker->search_min, multiplier);
  mul_v2_fl(marker->search_max, multiplier);
}

/* Helper function for BKE_tracking_tracks_average which takes care of averaging fields of
 * markers (position, patterns, ...). */
static void tracking_average_markers(MovieTrackingTrack *dst_track,
                                     /*const*/ MovieTrackingTrack **src_tracks,
                                     const int num_src_tracks)
{
  /* Get global range of frames within which averaging would happen. */
  int first_frame, last_frame;
  BKE_tracking_tracks_first_last_frame_minmax(
      src_tracks, num_src_tracks, &first_frame, &last_frame);
  if (last_frame < first_frame) {
    return;
  }
  const int num_frames = last_frame - first_frame + 1;

  /* Allocate temporary array where averaging will happen into. */
  MovieTrackingMarker *accumulator = MEM_calloc_arrayN(
      num_frames, sizeof(MovieTrackingMarker), "tracks average accumulator");
  int *counters = MEM_calloc_arrayN(num_frames, sizeof(int), "tracks accumulator counters");
  for (int frame = first_frame; frame <= last_frame; ++frame) {
    const int frame_index = frame - first_frame;
    accumulator[frame_index].framenr = frame;
    accumulator[frame_index].flag |= (MARKER_DISABLED | MARKER_TRACKED);
  }

  /* Accumulate track markers. */
  for (int track_index = 0; track_index < num_src_tracks; ++track_index) {
    /*const*/ MovieTrackingTrack *track = src_tracks[track_index];
    for (int frame = first_frame; frame <= last_frame; ++frame) {
      MovieTrackingMarker interpolated_marker;
      if (!BKE_tracking_marker_get_interpolated(track, frame, &interpolated_marker)) {
        continue;
      }
      const int frame_index = frame - first_frame;
      accumulate_marker(&accumulator[frame_index], &interpolated_marker);
      ++counters[frame_index];
    }
  }

  /* Average and store the result. */
  for (int frame = first_frame; frame <= last_frame; ++frame) {
    /* Average. */
    const int frame_index = frame - first_frame;
    if (!counters[frame_index]) {
      continue;
    }
    const float multiplier = 1.0f / (float)counters[frame_index];
    multiply_marker(&accumulator[frame_index], multiplier);
    /* Store the result. */
    BKE_tracking_marker_insert(dst_track, &accumulator[frame_index]);
  }

  /* Free memory. */
  MEM_freeN(accumulator);
  MEM_freeN(counters);
}

/* Helper function for BKE_tracking_tracks_average which takes care of averaging fields of
 * tracks (track for example, offset). */
static void tracking_average_tracks(MovieTrackingTrack *dst_track,
                                    /*const*/ MovieTrackingTrack **src_tracks,
                                    const int num_src_tracks)
{
  /* TODO(sergey): Consider averaging weight, stabilization weight, maybe even bundle position. */
  zero_v2(dst_track->offset);
  for (int track_index = 0; track_index < num_src_tracks; track_index++) {
    add_v2_v2(dst_track->offset, src_tracks[track_index]->offset);
  }
  mul_v2_fl(dst_track->offset, 1.0f / num_src_tracks);
}

void BKE_tracking_tracks_average(MovieTrackingTrack *dst_track,
                                 /*const*/ MovieTrackingTrack **src_tracks,
                                 const int num_src_tracks)
{
  if (num_src_tracks == 0) {
    return;
  }

  tracking_average_markers(dst_track, src_tracks, num_src_tracks);
  tracking_average_tracks(dst_track, src_tracks, num_src_tracks);
}

MovieTrackingTrack *BKE_tracking_track_get_named(MovieTracking *tracking,
                                                 MovieTrackingObject *object,
                                                 const char *name)
{
  ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);
  MovieTrackingTrack *track = tracksbase->first;

  while (track) {
    if (STREQ(track->name, name)) {
      return track;
    }

    track = track->next;
  }

  return NULL;
}

MovieTrackingTrack *BKE_tracking_track_get_indexed(MovieTracking *tracking,
                                                   int tracknr,
                                                   ListBase **r_tracksbase)
{
  MovieTrackingObject *object;
  int cur = 1;

  object = tracking->objects.first;
  while (object) {
    ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);
    MovieTrackingTrack *track = tracksbase->first;

    while (track) {
      if (track->flag & TRACK_HAS_BUNDLE) {
        if (cur == tracknr) {
          *r_tracksbase = tracksbase;
          return track;
        }

        cur++;
      }

      track = track->next;
    }

    object = object->next;
  }

  *r_tracksbase = NULL;

  return NULL;
}

MovieTrackingTrack *BKE_tracking_track_get_active(MovieTracking *tracking)
{
  ListBase *tracksbase;

  if (!tracking->act_track) {
    return NULL;
  }

  tracksbase = BKE_tracking_get_active_tracks(tracking);

  /* check that active track is in current tracks list */
  if (BLI_findindex(tracksbase, tracking->act_track) != -1) {
    return tracking->act_track;
  }

  return NULL;
}

static bGPDlayer *track_mask_gpencil_layer_get(MovieTrackingTrack *track)
{
  bGPDlayer *layer;

  if (!track->gpd) {
    return NULL;
  }

  layer = track->gpd->layers.first;

  while (layer) {
    if (layer->flag & GP_LAYER_ACTIVE) {
      bGPDframe *frame = layer->frames.first;
      bool ok = false;

      while (frame) {
        if (frame->strokes.first) {
          ok = true;
          break;
        }

        frame = frame->next;
      }

      if (ok) {
        return layer;
      }
    }

    layer = layer->next;
  }

  return NULL;
}

typedef struct TrackMaskSetPixelData {
  float *mask;
  int mask_width;
  int mask_height;
} TrackMaskSetPixelData;

static void track_mask_set_pixel_cb(int x, int x_end, int y, void *user_data)
{
  TrackMaskSetPixelData *data = (TrackMaskSetPixelData *)user_data;
  size_t index = (size_t)y * data->mask_width + x;
  size_t index_end = (size_t)y * data->mask_width + x_end;
  do {
    data->mask[index] = 1.0f;
  } while (++index != index_end);
}

static void track_mask_gpencil_layer_rasterize(int frame_width,
                                               int frame_height,
                                               const float region_min[2],
                                               bGPDlayer *layer,
                                               float *mask,
                                               int mask_width,
                                               int mask_height)
{
  bGPDframe *frame = layer->frames.first;
  TrackMaskSetPixelData data;

  data.mask = mask;
  data.mask_width = mask_width;
  data.mask_height = mask_height;

  while (frame) {
    bGPDstroke *stroke = frame->strokes.first;

    while (stroke) {
      bGPDspoint *stroke_points = stroke->points;
      if (stroke->flag & GP_STROKE_2DSPACE) {
        int *mask_points, *point;
        point = mask_points = MEM_callocN(2 * stroke->totpoints * sizeof(int),
                                          "track mask rasterization points");
        for (int i = 0; i < stroke->totpoints; i++, point += 2) {
          point[0] = stroke_points[i].x * frame_width - region_min[0];
          point[1] = stroke_points[i].y * frame_height - region_min[1];
        }
        /* TODO: add an option to control whether AA is enabled or not */
        BLI_bitmap_draw_2d_poly_v2i_n(0,
                                      0,
                                      mask_width,
                                      mask_height,
                                      (const int(*)[2])mask_points,
                                      stroke->totpoints,
                                      track_mask_set_pixel_cb,
                                      &data);
        MEM_freeN(mask_points);
      }
      stroke = stroke->next;
    }
    frame = frame->next;
  }
}

float *tracking_track_get_mask_for_region(int frame_width,
                                          int frame_height,
                                          const float region_min[2],
                                          const float region_max[2],
                                          MovieTrackingTrack *track)
{
  float *mask = NULL;
  bGPDlayer *layer = track_mask_gpencil_layer_get(track);
  if (layer != NULL) {
    const int mask_width = region_max[0] - region_min[0];
    const int mask_height = region_max[1] - region_min[1];
    mask = MEM_callocN(mask_width * mask_height * sizeof(float), "track mask");
    track_mask_gpencil_layer_rasterize(
        frame_width, frame_height, region_min, layer, mask, mask_width, mask_height);
  }
  return mask;
}

float *BKE_tracking_track_get_mask(int frame_width,
                                   int frame_height,
                                   MovieTrackingTrack *track,
                                   MovieTrackingMarker *marker)
{
  /* Convert normalized space marker's search area to pixel-space region. */
  const float region_min[2] = {
      marker->search_min[0] * frame_width,
      marker->search_min[1] * frame_height,
  };
  const float region_max[2] = {
      marker->search_max[0] * frame_width,
      marker->search_max[1] * frame_height,
  };
  return tracking_track_get_mask_for_region(
      frame_width, frame_height, region_min, region_max, track);
}

float BKE_tracking_track_get_weight_for_marker(MovieClip *clip,
                                               MovieTrackingTrack *track,
                                               MovieTrackingMarker *marker)
{
  FCurve *weight_fcurve;
  float weight = track->weight;

  weight_fcurve = id_data_find_fcurve(
      &clip->id, track, &RNA_MovieTrackingTrack, "weight", 0, NULL);

  if (weight_fcurve) {
    int scene_framenr = BKE_movieclip_remap_clip_to_scene_frame(clip, marker->framenr);
    weight = evaluate_fcurve(weight_fcurve, scene_framenr);
  }

  return weight;
}

void BKE_tracking_track_select(ListBase *tracksbase,
                               MovieTrackingTrack *track,
                               int area,
                               bool extend)
{
  if (extend) {
    BKE_tracking_track_flag_set(track, area, SELECT);
  }
  else {
    MovieTrackingTrack *cur = tracksbase->first;

    while (cur) {
      if ((cur->flag & TRACK_HIDDEN) == 0) {
        if (cur == track) {
          BKE_tracking_track_flag_clear(cur, TRACK_AREA_ALL, SELECT);
          BKE_tracking_track_flag_set(cur, area, SELECT);
        }
        else {
          BKE_tracking_track_flag_clear(cur, TRACK_AREA_ALL, SELECT);
        }
      }

      cur = cur->next;
    }
  }
}

void BKE_tracking_track_deselect(MovieTrackingTrack *track, int area)
{
  BKE_tracking_track_flag_clear(track, area, SELECT);
}

void BKE_tracking_tracks_deselect_all(ListBase *tracksbase)
{
  LISTBASE_FOREACH (MovieTrackingTrack *, track, tracksbase) {
    if ((track->flag & TRACK_HIDDEN) == 0) {
      BKE_tracking_track_flag_clear(track, TRACK_AREA_ALL, SELECT);
    }
  }
}

/* --------------------------------------------------------------------
 * Marker.
 */

MovieTrackingMarker *BKE_tracking_marker_insert(MovieTrackingTrack *track,
                                                MovieTrackingMarker *marker)
{
  MovieTrackingMarker *old_marker = NULL;

  if (track->markersnr) {
    old_marker = BKE_tracking_marker_get_exact(track, marker->framenr);
  }

  if (old_marker) {
    /* simply replace settings for already allocated marker */
    *old_marker = *marker;

    return old_marker;
  }

  int a = track->markersnr;

  /* find position in array where to add new marker */
  while (a--) {
    if (track->markers[a].framenr < marker->framenr) {
      break;
    }
  }

  track->markersnr++;

  if (track->markers) {
    track->markers = MEM_reallocN(track->markers, sizeof(MovieTrackingMarker) * track->markersnr);
  }
  else {
    track->markers = MEM_callocN(sizeof(MovieTrackingMarker), "MovieTracking markers");
  }

  /* shift array to "free" space for new marker */
  memmove(track->markers + a + 2,
          track->markers + a + 1,
          (track->markersnr - a - 2) * sizeof(MovieTrackingMarker));

  /* put new marker */
  track->markers[a + 1] = *marker;

  return &track->markers[a + 1];
}

void BKE_tracking_marker_delete(MovieTrackingTrack *track, int framenr)
{
  int a = 0;

  while (a < track->markersnr) {
    if (track->markers[a].framenr == framenr) {
      if (track->markersnr > 1) {
        memmove(track->markers + a,
                track->markers + a + 1,
                (track->markersnr - a - 1) * sizeof(MovieTrackingMarker));
        track->markersnr--;
        track->markers = MEM_reallocN(track->markers,
                                      sizeof(MovieTrackingMarker) * track->markersnr);
      }
      else {
        MEM_freeN(track->markers);
        track->markers = NULL;
        track->markersnr = 0;
      }

      break;
    }

    a++;
  }
}

void BKE_tracking_marker_clamp_pattern_position(MovieTrackingMarker *marker)
{
  float pat_min[2], pat_max[2];
  BKE_tracking_marker_pattern_minmax(marker, pat_min, pat_max);

  for (int a = 0; a < 2; a++) {
    if (pat_min[a] < marker->search_min[a]) {
      for (int b = 0; b < 4; b++) {
        marker->pattern_corners[b][a] += marker->search_min[a] - pat_min[a];
      }
    }
    if (pat_max[a] > marker->search_max[a]) {
      for (int b = 0; b < 4; b++) {
        marker->pattern_corners[b][a] -= pat_max[a] - marker->search_max[a];
      }
    }
  }
}

void BKE_tracking_marker_clamp_search_size(MovieTrackingMarker *marker)
{
  float pat_min[2], pat_max[2];
  BKE_tracking_marker_pattern_minmax(marker, pat_min, pat_max);

  for (int a = 0; a < 2; a++) {
    marker->search_min[a] = min_ff(pat_min[a], marker->search_min[a]);
    marker->search_max[a] = max_ff(pat_max[a], marker->search_max[a]);
  }
}

void BKE_tracking_marker_clamp_search_position(MovieTrackingMarker *marker)
{
  float pat_min[2], pat_max[2];
  BKE_tracking_marker_pattern_minmax(marker, pat_min, pat_max);

  float dim[2];
  sub_v2_v2v2(dim, marker->search_max, marker->search_min);

  for (int a = 0; a < 2; a++) {
    if (marker->search_min[a] > pat_min[a]) {
      marker->search_min[a] = pat_min[a];
      marker->search_max[a] = marker->search_min[a] + dim[a];
    }
    if (marker->search_max[a] < pat_max[a]) {
      marker->search_max[a] = pat_max[a];
      marker->search_min[a] = marker->search_max[a] - dim[a];
    }
  }
}

MovieTrackingMarker *BKE_tracking_marker_get(MovieTrackingTrack *track, int framenr)
{
  const int num_markers = track->markersnr;

  if (num_markers == 0) {
    BLI_assert_msg(0, "Detected degenerated track, should never happen.");
    return NULL;
  }

  int left_boundary = 0;
  int right_boundary = num_markers;
  while (left_boundary < right_boundary) {
    const int median_index = (left_boundary + right_boundary) / 2;
    MovieTrackingMarker *marker = &track->markers[median_index];

    if (marker->framenr == framenr) {
      return marker;
    }

    if (marker->framenr < framenr) {
      left_boundary = median_index + 1;
    }
    else {
      BLI_assert(marker->framenr > framenr);
      right_boundary = median_index - 1;
    }
  }

  const int closest_index = clamp_i(right_boundary, 0, num_markers - 1);

  return &track->markers[closest_index];
}

MovieTrackingMarker *BKE_tracking_marker_get_exact(MovieTrackingTrack *track, int framenr)
{
  MovieTrackingMarker *marker = BKE_tracking_marker_get(track, framenr);

  if (marker->framenr != framenr) {
    return NULL;
  }

  return marker;
}

MovieTrackingMarker *BKE_tracking_marker_ensure(MovieTrackingTrack *track, int framenr)
{
  MovieTrackingMarker *marker = BKE_tracking_marker_get(track, framenr);

  if (marker->framenr != framenr) {
    MovieTrackingMarker marker_new;

    marker_new = *marker;
    marker_new.framenr = framenr;

    BKE_tracking_marker_insert(track, &marker_new);
    marker = BKE_tracking_marker_get(track, framenr);
  }

  return marker;
}

static const MovieTrackingMarker *get_usable_marker_for_interpolation(
    struct MovieTrackingTrack *track,
    const MovieTrackingMarker *anchor_marker,
    const int direction)
{
  BLI_assert(ELEM(direction, -1, 1));

  const MovieTrackingMarker *last_marker = track->markers + track->markersnr - 1;
  const MovieTrackingMarker *current_marker = anchor_marker;

  while (current_marker >= track->markers && current_marker <= last_marker) {
    if ((current_marker->flag & MARKER_DISABLED) == 0) {
      return current_marker;
    }
    current_marker += direction;
  }

  return NULL;
}

bool BKE_tracking_marker_get_interpolated(struct MovieTrackingTrack *track,
                                          const int framenr,
                                          struct MovieTrackingMarker *r_marker)
{
  const MovieTrackingMarker *closest_marker = BKE_tracking_marker_get(track, framenr);
  if (closest_marker == NULL) {
    return false;
  }
  if (closest_marker->framenr == framenr && (closest_marker->flag & MARKER_DISABLED) == 0) {
    *r_marker = *closest_marker;
    return true;
  }

  const MovieTrackingMarker *left_marker = get_usable_marker_for_interpolation(
      track, closest_marker, -1);
  if (left_marker == NULL) {
    return false;
  }

  const MovieTrackingMarker *right_marker = get_usable_marker_for_interpolation(
      track, closest_marker + 1, 1);
  if (right_marker == NULL) {
    return false;
  }

  if (left_marker == right_marker) {
    *r_marker = *left_marker;
    return true;
  }

  const float factor = (float)(framenr - left_marker->framenr) /
                       (right_marker->framenr - left_marker->framenr);

  interp_v2_v2v2(r_marker->pos, left_marker->pos, right_marker->pos, factor);

  for (int i = 0; i < 4; i++) {
    interp_v2_v2v2(r_marker->pattern_corners[i],
                   left_marker->pattern_corners[i],
                   right_marker->pattern_corners[i],
                   factor);
  }

  interp_v2_v2v2(r_marker->search_min, left_marker->search_min, right_marker->search_min, factor);
  interp_v2_v2v2(r_marker->search_max, left_marker->search_max, right_marker->search_max, factor);

  r_marker->framenr = framenr;
  r_marker->flag = 0;

  if (framenr == left_marker->framenr) {
    r_marker->flag = left_marker->flag;
  }
  else if (framenr == right_marker->framenr) {
    r_marker->flag = right_marker->flag;
  }

  return true;
}

void BKE_tracking_marker_pattern_minmax(const MovieTrackingMarker *marker,
                                        float min[2],
                                        float max[2])
{
  INIT_MINMAX2(min, max);

  minmax_v2v2_v2(min, max, marker->pattern_corners[0]);
  minmax_v2v2_v2(min, max, marker->pattern_corners[1]);
  minmax_v2v2_v2(min, max, marker->pattern_corners[2]);
  minmax_v2v2_v2(min, max, marker->pattern_corners[3]);
}

void BKE_tracking_marker_get_subframe_position(MovieTrackingTrack *track,
                                               float framenr,
                                               float pos[2])
{
  MovieTrackingMarker *marker = BKE_tracking_marker_get(track, (int)framenr);
  MovieTrackingMarker *marker_last = track->markers + (track->markersnr - 1);

  if (marker != marker_last) {
    MovieTrackingMarker *marker_next = marker + 1;

    if (marker_next->framenr == marker->framenr + 1) {
      /* currently only do subframing inside tracked ranges, do not extrapolate tracked segments
       * could be changed when / if mask parent would be interpolating position in-between
       * tracked segments
       */

      float fac = (framenr - (int)framenr) / (marker_next->framenr - marker->framenr);

      interp_v2_v2v2(pos, marker->pos, marker_next->pos, fac);
    }
    else {
      copy_v2_v2(pos, marker->pos);
    }
  }
  else {
    copy_v2_v2(pos, marker->pos);
  }

  /* currently track offset is always wanted to be applied here, could be made an option later */
  add_v2_v2(pos, track->offset);
}

/* --------------------------------------------------------------------
 * Plane track.
 */

MovieTrackingPlaneTrack *BKE_tracking_plane_track_add(MovieTracking *tracking,
                                                      ListBase *plane_tracks_base,
                                                      ListBase *tracks,
                                                      int framenr)
{
  MovieTrackingPlaneTrack *plane_track;
  MovieTrackingPlaneMarker plane_marker;
  float tracks_min[2], tracks_max[2];
  int num_selected_tracks = 0;

  (void)tracking; /* Ignored. */

  /* Use bounding box of selected markers as an initial size of plane. */
  INIT_MINMAX2(tracks_min, tracks_max);
  LISTBASE_FOREACH (MovieTrackingTrack *, track, tracks) {
    if (TRACK_SELECTED(track)) {
      MovieTrackingMarker *marker = BKE_tracking_marker_get(track, framenr);
      float pattern_min[2], pattern_max[2];
      BKE_tracking_marker_pattern_minmax(marker, pattern_min, pattern_max);
      add_v2_v2(pattern_min, marker->pos);
      add_v2_v2(pattern_max, marker->pos);
      minmax_v2v2_v2(tracks_min, tracks_max, pattern_min);
      minmax_v2v2_v2(tracks_min, tracks_max, pattern_max);
      num_selected_tracks++;
    }
  }

  if (num_selected_tracks < 4) {
    return NULL;
  }

  /* Allocate new plane track. */
  plane_track = MEM_callocN(sizeof(MovieTrackingPlaneTrack), "new plane track");

  /* Use some default name. */
  strcpy(plane_track->name, "Plane Track");

  plane_track->image_opacity = 1.0f;

  /* Use selected tracks from given list as a plane. */
  plane_track->point_tracks = MEM_mallocN(sizeof(MovieTrackingTrack *) * num_selected_tracks,
                                          "new plane tracks array");
  int track_index = 0;
  LISTBASE_FOREACH (MovieTrackingTrack *, track, tracks) {
    if (TRACK_SELECTED(track)) {
      plane_track->point_tracks[track_index] = track;
      track_index++;
    }
  }
  plane_track->point_tracksnr = num_selected_tracks;

  /* Setup new plane marker and add it to the track. */
  plane_marker.framenr = framenr;
  plane_marker.flag = 0;

  copy_v2_v2(plane_marker.corners[0], tracks_min);
  copy_v2_v2(plane_marker.corners[2], tracks_max);

  plane_marker.corners[1][0] = tracks_max[0];
  plane_marker.corners[1][1] = tracks_min[1];
  plane_marker.corners[3][0] = tracks_min[0];
  plane_marker.corners[3][1] = tracks_max[1];

  BKE_tracking_plane_marker_insert(plane_track, &plane_marker);

  /* Put new plane track to the list, ensure its name is unique. */
  BLI_addtail(plane_tracks_base, plane_track);
  BKE_tracking_plane_track_unique_name(plane_tracks_base, plane_track);

  return plane_track;
}

void BKE_tracking_plane_track_unique_name(ListBase *plane_tracks_base,
                                          MovieTrackingPlaneTrack *plane_track)
{
  BLI_uniquename(plane_tracks_base,
                 plane_track,
                 CTX_DATA_(BLT_I18NCONTEXT_ID_MOVIECLIP, "Plane Track"),
                 '.',
                 offsetof(MovieTrackingPlaneTrack, name),
                 sizeof(plane_track->name));
}

void BKE_tracking_plane_track_free(MovieTrackingPlaneTrack *plane_track)
{
  if (plane_track->markers) {
    MEM_freeN(plane_track->markers);
  }

  MEM_freeN(plane_track->point_tracks);
}

MovieTrackingPlaneTrack *BKE_tracking_plane_track_get_named(MovieTracking *tracking,
                                                            MovieTrackingObject *object,
                                                            const char *name)
{
  ListBase *plane_tracks_base = BKE_tracking_object_get_plane_tracks(tracking, object);

  LISTBASE_FOREACH (MovieTrackingPlaneTrack *, plane_track, plane_tracks_base) {
    if (STREQ(plane_track->name, name)) {
      return plane_track;
    }
  }

  return NULL;
}

MovieTrackingPlaneTrack *BKE_tracking_plane_track_get_active(struct MovieTracking *tracking)
{
  if (tracking->act_plane_track == NULL) {
    return NULL;
  }

  ListBase *plane_tracks_base = BKE_tracking_get_active_plane_tracks(tracking);

  /* Check that active track is in current plane tracks list */
  if (BLI_findindex(plane_tracks_base, tracking->act_plane_track) != -1) {
    return tracking->act_plane_track;
  }

  return NULL;
}

void BKE_tracking_plane_tracks_deselect_all(ListBase *plane_tracks_base)
{
  LISTBASE_FOREACH (MovieTrackingPlaneTrack *, plane_track, plane_tracks_base) {
    plane_track->flag &= ~SELECT;
  }
}

bool BKE_tracking_plane_track_has_point_track(MovieTrackingPlaneTrack *plane_track,
                                              MovieTrackingTrack *track)
{
  for (int i = 0; i < plane_track->point_tracksnr; i++) {
    if (plane_track->point_tracks[i] == track) {
      return true;
    }
  }
  return false;
}

bool BKE_tracking_plane_track_remove_point_track(MovieTrackingPlaneTrack *plane_track,
                                                 MovieTrackingTrack *track)
{
  if (plane_track->point_tracksnr <= 4) {
    return false;
  }

  MovieTrackingTrack **new_point_tracks = MEM_mallocN(
      sizeof(*new_point_tracks) * (plane_track->point_tracksnr - 1), "new point tracks array");

  for (int i = 0, track_index = 0; i < plane_track->point_tracksnr; i++) {
    if (plane_track->point_tracks[i] != track) {
      new_point_tracks[track_index++] = plane_track->point_tracks[i];
    }
  }

  MEM_freeN(plane_track->point_tracks);
  plane_track->point_tracks = new_point_tracks;
  plane_track->point_tracksnr--;

  return true;
}

void BKE_tracking_plane_tracks_remove_point_track(MovieTracking *tracking,
                                                  MovieTrackingTrack *track)
{
  ListBase *plane_tracks_base = BKE_tracking_get_active_plane_tracks(tracking);
  LISTBASE_FOREACH_MUTABLE (MovieTrackingPlaneTrack *, plane_track, plane_tracks_base) {
    if (BKE_tracking_plane_track_has_point_track(plane_track, track)) {
      if (!BKE_tracking_plane_track_remove_point_track(plane_track, track)) {
        /* Delete planes with less than 3 point tracks in it. */
        BKE_tracking_plane_track_free(plane_track);
        BLI_freelinkN(plane_tracks_base, plane_track);
      }
    }
  }
}

void BKE_tracking_plane_track_replace_point_track(MovieTrackingPlaneTrack *plane_track,
                                                  MovieTrackingTrack *old_track,
                                                  MovieTrackingTrack *new_track)
{
  for (int i = 0; i < plane_track->point_tracksnr; i++) {
    if (plane_track->point_tracks[i] == old_track) {
      plane_track->point_tracks[i] = new_track;
      break;
    }
  }
}

void BKE_tracking_plane_tracks_replace_point_track(MovieTracking *tracking,
                                                   MovieTrackingTrack *old_track,
                                                   MovieTrackingTrack *new_track)
{
  ListBase *plane_tracks_base = BKE_tracking_get_active_plane_tracks(tracking);
  LISTBASE_FOREACH (MovieTrackingPlaneTrack *, plane_track, plane_tracks_base) {
    if (BKE_tracking_plane_track_has_point_track(plane_track, old_track)) {
      BKE_tracking_plane_track_replace_point_track(plane_track, old_track, new_track);
    }
  }
}

/* --------------------------------------------------------------------
 * Plane marker.
 */

MovieTrackingPlaneMarker *BKE_tracking_plane_marker_insert(MovieTrackingPlaneTrack *plane_track,
                                                           MovieTrackingPlaneMarker *plane_marker)
{
  MovieTrackingPlaneMarker *old_plane_marker = NULL;

  if (plane_track->markersnr) {
    old_plane_marker = BKE_tracking_plane_marker_get_exact(plane_track, plane_marker->framenr);
  }

  if (old_plane_marker) {
    /* Simply replace settings in existing marker. */
    *old_plane_marker = *plane_marker;

    return old_plane_marker;
  }

  int a = plane_track->markersnr;

  /* Find position in array where to add new marker. */
  /* TODO(sergey): we could use bisect to speed things up. */
  while (a--) {
    if (plane_track->markers[a].framenr < plane_marker->framenr) {
      break;
    }
  }

  plane_track->markersnr++;
  plane_track->markers = MEM_reallocN(plane_track->markers,
                                      sizeof(MovieTrackingPlaneMarker) * plane_track->markersnr);

  /* Shift array to "free" space for new marker. */
  memmove(plane_track->markers + a + 2,
          plane_track->markers + a + 1,
          (plane_track->markersnr - a - 2) * sizeof(MovieTrackingPlaneMarker));

  /* Put new marker to an array. */
  plane_track->markers[a + 1] = *plane_marker;

  return &plane_track->markers[a + 1];
}

void BKE_tracking_plane_marker_delete(MovieTrackingPlaneTrack *plane_track, int framenr)
{
  int a = 0;

  while (a < plane_track->markersnr) {
    if (plane_track->markers[a].framenr == framenr) {
      if (plane_track->markersnr > 1) {
        memmove(plane_track->markers + a,
                plane_track->markers + a + 1,
                (plane_track->markersnr - a - 1) * sizeof(MovieTrackingPlaneMarker));
        plane_track->markersnr--;
        plane_track->markers = MEM_reallocN(plane_track->markers,
                                            sizeof(MovieTrackingMarker) * plane_track->markersnr);
      }
      else {
        MEM_freeN(plane_track->markers);
        plane_track->markers = NULL;
        plane_track->markersnr = 0;
      }

      break;
    }

    a++;
  }
}

/* TODO(sergey): The next couple of functions are really quite the same as point marker version,
 *               would be nice to de-duplicate them somehow..
 */

MovieTrackingPlaneMarker *BKE_tracking_plane_marker_get(MovieTrackingPlaneTrack *plane_track,
                                                        int framenr)
{
  int a = plane_track->markersnr - 1;

  if (!plane_track->markersnr) {
    return NULL;
  }

  /* Approximate pre-first framenr marker with first marker. */
  if (framenr < plane_track->markers[0].framenr) {
    return &plane_track->markers[0];
  }

  if (plane_track->last_marker < plane_track->markersnr) {
    a = plane_track->last_marker;
  }

  if (plane_track->markers[a].framenr <= framenr) {
    while (a < plane_track->markersnr && plane_track->markers[a].framenr <= framenr) {
      if (plane_track->markers[a].framenr == framenr) {
        plane_track->last_marker = a;

        return &plane_track->markers[a];
      }
      a++;
    }

    /* If there's no marker for exact position, use nearest marker from left side. */
    return &plane_track->markers[a - 1];
  }

  while (a >= 0 && plane_track->markers[a].framenr >= framenr) {
    if (plane_track->markers[a].framenr == framenr) {
      plane_track->last_marker = a;

      return &plane_track->markers[a];
    }

    a--;
  }

  /* If there's no marker for exact position, use nearest marker from left side. */
  return &plane_track->markers[a];

  return NULL;
}

MovieTrackingPlaneMarker *BKE_tracking_plane_marker_get_exact(MovieTrackingPlaneTrack *plane_track,
                                                              int framenr)
{
  MovieTrackingPlaneMarker *plane_marker = BKE_tracking_plane_marker_get(plane_track, framenr);

  if (plane_marker->framenr != framenr) {
    return NULL;
  }

  return plane_marker;
}

MovieTrackingPlaneMarker *BKE_tracking_plane_marker_ensure(MovieTrackingPlaneTrack *plane_track,
                                                           int framenr)
{
  MovieTrackingPlaneMarker *plane_marker = BKE_tracking_plane_marker_get(plane_track, framenr);

  if (plane_marker->framenr != framenr) {
    MovieTrackingPlaneMarker plane_marker_new;

    plane_marker_new = *plane_marker;
    plane_marker_new.framenr = framenr;

    plane_marker = BKE_tracking_plane_marker_insert(plane_track, &plane_marker_new);
  }

  return plane_marker;
}

void BKE_tracking_plane_marker_get_subframe_corners(MovieTrackingPlaneTrack *plane_track,
                                                    float framenr,
                                                    float corners[4][2])
{
  MovieTrackingPlaneMarker *marker = BKE_tracking_plane_marker_get(plane_track, (int)framenr);
  MovieTrackingPlaneMarker *marker_last = plane_track->markers + (plane_track->markersnr - 1);
  if (marker != marker_last) {
    MovieTrackingPlaneMarker *marker_next = marker + 1;
    if (marker_next->framenr == marker->framenr + 1) {
      float fac = (framenr - (int)framenr) / (marker_next->framenr - marker->framenr);
      for (int i = 0; i < 4; i++) {
        interp_v2_v2v2(corners[i], marker->corners[i], marker_next->corners[i], fac);
      }
    }
    else {
      for (int i = 0; i < 4; i++) {
        copy_v2_v2(corners[i], marker->corners[i]);
      }
    }
  }
  else {
    for (int i = 0; i < 4; i++) {
      copy_v2_v2(corners[i], marker->corners[i]);
    }
  }
}

/* --------------------------------------------------------------------
 * Object.
 */

MovieTrackingObject *BKE_tracking_object_add(MovieTracking *tracking, const char *name)
{
  MovieTrackingObject *object = MEM_callocN(sizeof(MovieTrackingObject), "tracking object");

  if (tracking->tot_object == 0) {
    /* first object is always camera */
    BLI_strncpy(object->name, "Camera", sizeof(object->name));

    object->flag |= TRACKING_OBJECT_CAMERA;
  }
  else {
    BLI_strncpy(object->name, name, sizeof(object->name));
  }

  BLI_addtail(&tracking->objects, object);

  tracking->tot_object++;
  tracking->objectnr = BLI_listbase_count(&tracking->objects) - 1;

  object->scale = 1.0f;
  object->keyframe1 = 1;
  object->keyframe2 = 30;

  BKE_tracking_object_unique_name(tracking, object);
  BKE_tracking_dopesheet_tag_update(tracking);

  return object;
}

bool BKE_tracking_object_delete(MovieTracking *tracking, MovieTrackingObject *object)
{
  MovieTrackingTrack *track;
  int index = BLI_findindex(&tracking->objects, object);

  if (index == -1) {
    return false;
  }

  if (object->flag & TRACKING_OBJECT_CAMERA) {
    /* object used for camera solving can't be deleted */
    return false;
  }

  track = object->tracks.first;
  while (track) {
    if (track == tracking->act_track) {
      tracking->act_track = NULL;
    }

    track = track->next;
  }

  tracking_object_free(object);
  BLI_freelinkN(&tracking->objects, object);

  tracking->tot_object--;

  if (index != 0) {
    tracking->objectnr = index - 1;
  }
  else {
    tracking->objectnr = 0;
  }

  BKE_tracking_dopesheet_tag_update(tracking);

  return true;
}

void BKE_tracking_object_unique_name(MovieTracking *tracking, MovieTrackingObject *object)
{
  BLI_uniquename(&tracking->objects,
                 object,
                 DATA_("Object"),
                 '.',
                 offsetof(MovieTrackingObject, name),
                 sizeof(object->name));
}

MovieTrackingObject *BKE_tracking_object_get_named(MovieTracking *tracking, const char *name)
{
  MovieTrackingObject *object = tracking->objects.first;

  while (object) {
    if (STREQ(object->name, name)) {
      return object;
    }

    object = object->next;
  }

  return NULL;
}

MovieTrackingObject *BKE_tracking_object_get_active(MovieTracking *tracking)
{
  return BLI_findlink(&tracking->objects, tracking->objectnr);
}

MovieTrackingObject *BKE_tracking_object_get_camera(MovieTracking *tracking)
{
  MovieTrackingObject *object = tracking->objects.first;

  while (object) {
    if (object->flag & TRACKING_OBJECT_CAMERA) {
      return object;
    }

    object = object->next;
  }

  return NULL;
}

ListBase *BKE_tracking_object_get_tracks(MovieTracking *tracking, MovieTrackingObject *object)
{
  if (object->flag & TRACKING_OBJECT_CAMERA) {
    return &tracking->tracks;
  }

  return &object->tracks;
}

ListBase *BKE_tracking_object_get_plane_tracks(MovieTracking *tracking,
                                               MovieTrackingObject *object)
{
  if (object->flag & TRACKING_OBJECT_CAMERA) {
    return &tracking->plane_tracks;
  }

  return &object->plane_tracks;
}

MovieTrackingReconstruction *BKE_tracking_object_get_reconstruction(MovieTracking *tracking,
                                                                    MovieTrackingObject *object)
{
  if (object->flag & TRACKING_OBJECT_CAMERA) {
    return &tracking->reconstruction;
  }

  return &object->reconstruction;
}

/* --------------------------------------------------------------------
 * Camera.
 */

static int reconstructed_camera_index_get(MovieTrackingReconstruction *reconstruction,
                                          int framenr,
                                          bool nearest)
{
  MovieReconstructedCamera *cameras = reconstruction->cameras;
  int a = 0, d = 1;

  if (!reconstruction->camnr) {
    return -1;
  }

  if (framenr < cameras[0].framenr) {
    if (nearest) {
      return 0;
    }

    return -1;
  }

  if (framenr > cameras[reconstruction->camnr - 1].framenr) {
    if (nearest) {
      return reconstruction->camnr - 1;
    }

    return -1;
  }

  if (reconstruction->last_camera < reconstruction->camnr) {
    a = reconstruction->last_camera;
  }

  if (cameras[a].framenr >= framenr) {
    d = -1;
  }

  while (a >= 0 && a < reconstruction->camnr) {
    int cfra = cameras[a].framenr;

    /* check if needed framenr was "skipped" -- no data for requested frame */

    if (d > 0 && cfra > framenr) {
      /* interpolate with previous position */
      if (nearest) {
        return a - 1;
      }

      break;
    }

    if (d < 0 && cfra < framenr) {
      /* interpolate with next position */
      if (nearest) {
        return a;
      }

      break;
    }

    if (cfra == framenr) {
      reconstruction->last_camera = a;

      return a;
    }

    a += d;
  }

  return -1;
}

static void reconstructed_camera_scale_set(MovieTrackingObject *object, float mat[4][4])
{
  if ((object->flag & TRACKING_OBJECT_CAMERA) == 0) {
    float smat[4][4];

    scale_m4_fl(smat, 1.0f / object->scale);
    mul_m4_m4m4(mat, mat, smat);
  }
}

void BKE_tracking_camera_shift_get(
    MovieTracking *tracking, int winx, int winy, float *shiftx, float *shifty)
{
  /* Indeed in both of cases it should be winx -
   * it's just how camera shift works for blender's camera. */
  *shiftx = (0.5f * winx - tracking->camera.principal[0]) / winx;
  *shifty = (0.5f * winy - tracking->camera.principal[1]) / winx;
}

void BKE_tracking_camera_to_blender(
    MovieTracking *tracking, Scene *scene, Camera *camera, int width, int height)
{
  float focal = tracking->camera.focal;

  camera->sensor_x = tracking->camera.sensor_width;
  camera->sensor_fit = CAMERA_SENSOR_FIT_AUTO;
  camera->lens = focal * camera->sensor_x / width;

  scene->r.xsch = width;
  scene->r.ysch = height;

  scene->r.xasp = tracking->camera.pixel_aspect;
  scene->r.yasp = 1.0f;

  BKE_tracking_camera_shift_get(tracking, width, height, &camera->shiftx, &camera->shifty);
}

MovieReconstructedCamera *BKE_tracking_camera_get_reconstructed(MovieTracking *tracking,
                                                                MovieTrackingObject *object,
                                                                int framenr)
{
  MovieTrackingReconstruction *reconstruction;
  int a;

  reconstruction = BKE_tracking_object_get_reconstruction(tracking, object);
  a = reconstructed_camera_index_get(reconstruction, framenr, false);

  if (a == -1) {
    return NULL;
  }

  return &reconstruction->cameras[a];
}

void BKE_tracking_camera_get_reconstructed_interpolate(MovieTracking *tracking,
                                                       MovieTrackingObject *object,
                                                       float framenr,
                                                       float mat[4][4])
{
  MovieTrackingReconstruction *reconstruction;
  MovieReconstructedCamera *cameras;
  int a;

  reconstruction = BKE_tracking_object_get_reconstruction(tracking, object);
  cameras = reconstruction->cameras;
  a = reconstructed_camera_index_get(reconstruction, (int)framenr, true);

  if (a == -1) {
    unit_m4(mat);
    return;
  }

  if (cameras[a].framenr != framenr && a < reconstruction->camnr - 1) {
    float t = ((float)framenr - cameras[a].framenr) /
              (cameras[a + 1].framenr - cameras[a].framenr);
    blend_m4_m4m4(mat, cameras[a].mat, cameras[a + 1].mat, t);
  }
  else {
    copy_m4_m4(mat, cameras[a].mat);
  }

  reconstructed_camera_scale_set(object, mat);
}

/* --------------------------------------------------------------------
 * (Un)distortion.
 */

MovieDistortion *BKE_tracking_distortion_new(MovieTracking *tracking,
                                             int calibration_width,
                                             int calibration_height)
{
  MovieDistortion *distortion;
  libmv_CameraIntrinsicsOptions camera_intrinsics_options;

  tracking_cameraIntrinscisOptionsFromTracking(
      tracking, calibration_width, calibration_height, &camera_intrinsics_options);

  distortion = MEM_callocN(sizeof(MovieDistortion), "BKE_tracking_distortion_create");
  distortion->intrinsics = libmv_cameraIntrinsicsNew(&camera_intrinsics_options);

  const MovieTrackingCamera *camera = &tracking->camera;
  copy_v2_v2(distortion->principal, camera->principal);
  distortion->pixel_aspect = camera->pixel_aspect;
  distortion->focal = camera->focal;

  return distortion;
}

void BKE_tracking_distortion_update(MovieDistortion *distortion,
                                    MovieTracking *tracking,
                                    int calibration_width,
                                    int calibration_height)
{
  libmv_CameraIntrinsicsOptions camera_intrinsics_options;

  tracking_cameraIntrinscisOptionsFromTracking(
      tracking, calibration_width, calibration_height, &camera_intrinsics_options);

  const MovieTrackingCamera *camera = &tracking->camera;
  copy_v2_v2(distortion->principal, camera->principal);
  distortion->pixel_aspect = camera->pixel_aspect;
  distortion->focal = camera->focal;

  libmv_cameraIntrinsicsUpdate(&camera_intrinsics_options, distortion->intrinsics);
}

void BKE_tracking_distortion_set_threads(MovieDistortion *distortion, int threads)
{
  libmv_cameraIntrinsicsSetThreads(distortion->intrinsics, threads);
}

MovieDistortion *BKE_tracking_distortion_copy(MovieDistortion *distortion)
{
  MovieDistortion *new_distortion;

  new_distortion = MEM_callocN(sizeof(MovieDistortion), "BKE_tracking_distortion_create");
  *new_distortion = *distortion;
  new_distortion->intrinsics = libmv_cameraIntrinsicsCopy(distortion->intrinsics);

  return new_distortion;
}

ImBuf *BKE_tracking_distortion_exec(MovieDistortion *distortion,
                                    MovieTracking *tracking,
                                    ImBuf *ibuf,
                                    int calibration_width,
                                    int calibration_height,
                                    float overscan,
                                    bool undistort)
{
  ImBuf *resibuf;

  BKE_tracking_distortion_update(distortion, tracking, calibration_width, calibration_height);

  resibuf = IMB_dupImBuf(ibuf);

  if (ibuf->rect_float) {
    if (undistort) {
      libmv_cameraIntrinsicsUndistortFloat(distortion->intrinsics,
                                           ibuf->rect_float,
                                           ibuf->x,
                                           ibuf->y,
                                           overscan,
                                           ibuf->channels,
                                           resibuf->rect_float);
    }
    else {
      libmv_cameraIntrinsicsDistortFloat(distortion->intrinsics,
                                         ibuf->rect_float,
                                         ibuf->x,
                                         ibuf->y,
                                         overscan,
                                         ibuf->channels,
                                         resibuf->rect_float);
    }

    if (ibuf->rect) {
      imb_freerectImBuf(ibuf);
    }
  }
  else {
    if (undistort) {
      libmv_cameraIntrinsicsUndistortByte(distortion->intrinsics,
                                          (uchar *)ibuf->rect,
                                          ibuf->x,
                                          ibuf->y,
                                          overscan,
                                          ibuf->channels,
                                          (uchar *)resibuf->rect);
    }
    else {
      libmv_cameraIntrinsicsDistortByte(distortion->intrinsics,
                                        (uchar *)ibuf->rect,
                                        ibuf->x,
                                        ibuf->y,
                                        overscan,
                                        ibuf->channels,
                                        (uchar *)resibuf->rect);
    }
  }

  return resibuf;
}

void BKE_tracking_distortion_distort_v2(MovieDistortion *distortion,
                                        const float co[2],
                                        float r_co[2])
{
  const float aspy = 1.0f / distortion->pixel_aspect;

  /* Normalize coords. */
  float inv_focal = 1.0f / distortion->focal;
  double x = (co[0] - distortion->principal[0]) * inv_focal,
         y = (co[1] - distortion->principal[1] * aspy) * inv_focal;

  libmv_cameraIntrinsicsApply(distortion->intrinsics, x, y, &x, &y);

  /* Result is in image coords already. */
  r_co[0] = x;
  r_co[1] = y;
}

void BKE_tracking_distortion_undistort_v2(MovieDistortion *distortion,
                                          const float co[2],
                                          float r_co[2])
{
  double x = co[0], y = co[1];
  libmv_cameraIntrinsicsInvert(distortion->intrinsics, x, y, &x, &y);

  const float aspy = 1.0f / distortion->pixel_aspect;
  r_co[0] = (float)x * distortion->focal + distortion->principal[0];
  r_co[1] = (float)y * distortion->focal + distortion->principal[1] * aspy;
}

void BKE_tracking_distortion_free(MovieDistortion *distortion)
{
  libmv_cameraIntrinsicsDestroy(distortion->intrinsics);

  MEM_freeN(distortion);
}

void BKE_tracking_distort_v2(
    MovieTracking *tracking, int image_width, int image_height, const float co[2], float r_co[2])
{
  const MovieTrackingCamera *camera = &tracking->camera;
  const float aspy = 1.0f / tracking->camera.pixel_aspect;

  libmv_CameraIntrinsicsOptions camera_intrinsics_options;
  tracking_cameraIntrinscisOptionsFromTracking(
      tracking, image_width, image_height, &camera_intrinsics_options);
  libmv_CameraIntrinsics *intrinsics = libmv_cameraIntrinsicsNew(&camera_intrinsics_options);

  /* Normalize coordinates. */
  double x = (co[0] - camera->principal[0]) / camera->focal,
         y = (co[1] - camera->principal[1] * aspy) / camera->focal;

  libmv_cameraIntrinsicsApply(intrinsics, x, y, &x, &y);
  libmv_cameraIntrinsicsDestroy(intrinsics);

  /* Result is in image coords already. */
  r_co[0] = x;
  r_co[1] = y;
}

void BKE_tracking_undistort_v2(
    MovieTracking *tracking, int image_width, int image_height, const float co[2], float r_co[2])
{
  const MovieTrackingCamera *camera = &tracking->camera;
  const float aspy = 1.0f / tracking->camera.pixel_aspect;

  libmv_CameraIntrinsicsOptions camera_intrinsics_options;
  tracking_cameraIntrinscisOptionsFromTracking(
      tracking, image_width, image_height, &camera_intrinsics_options);
  libmv_CameraIntrinsics *intrinsics = libmv_cameraIntrinsicsNew(&camera_intrinsics_options);

  double x = co[0], y = co[1];
  libmv_cameraIntrinsicsInvert(intrinsics, x, y, &x, &y);
  libmv_cameraIntrinsicsDestroy(intrinsics);

  r_co[0] = (float)x * camera->focal + camera->principal[0];
  r_co[1] = (float)y * camera->focal + camera->principal[1] * aspy;
}

ImBuf *BKE_tracking_undistort_frame(MovieTracking *tracking,
                                    ImBuf *ibuf,
                                    int calibration_width,
                                    int calibration_height,
                                    float overscan)
{
  MovieTrackingCamera *camera = &tracking->camera;

  if (camera->intrinsics == NULL) {
    camera->intrinsics = BKE_tracking_distortion_new(
        tracking, calibration_width, calibration_height);
  }

  return BKE_tracking_distortion_exec(
      camera->intrinsics, tracking, ibuf, calibration_width, calibration_height, overscan, true);
}

ImBuf *BKE_tracking_distort_frame(MovieTracking *tracking,
                                  ImBuf *ibuf,
                                  int calibration_width,
                                  int calibration_height,
                                  float overscan)
{
  MovieTrackingCamera *camera = &tracking->camera;

  if (camera->intrinsics == NULL) {
    camera->intrinsics = BKE_tracking_distortion_new(
        tracking, calibration_width, calibration_height);
  }

  return BKE_tracking_distortion_exec(
      camera->intrinsics, tracking, ibuf, calibration_width, calibration_height, overscan, false);
}

void BKE_tracking_max_distortion_delta_across_bound(MovieTracking *tracking,
                                                    int image_width,
                                                    int image_height,
                                                    rcti *rect,
                                                    bool undistort,
                                                    float delta[2])
{
  float pos[2], warped_pos[2];
  const int coord_delta = 5;
  void (*apply_distortion)(MovieTracking * tracking,
                           int image_width,
                           int image_height,
                           const float pos[2],
                           float out[2]);

  if (undistort) {
    apply_distortion = BKE_tracking_undistort_v2;
  }
  else {
    apply_distortion = BKE_tracking_distort_v2;
  }

  delta[0] = delta[1] = -FLT_MAX;

  for (int a = rect->xmin; a <= rect->xmax + coord_delta; a += coord_delta) {
    if (a > rect->xmax) {
      a = rect->xmax;
    }

    /* bottom edge */
    pos[0] = a;
    pos[1] = rect->ymin;

    apply_distortion(tracking, image_width, image_height, pos, warped_pos);

    delta[0] = max_ff(delta[0], fabsf(pos[0] - warped_pos[0]));
    delta[1] = max_ff(delta[1], fabsf(pos[1] - warped_pos[1]));

    /* top edge */
    pos[0] = a;
    pos[1] = rect->ymax;

    apply_distortion(tracking, image_width, image_height, pos, warped_pos);

    delta[0] = max_ff(delta[0], fabsf(pos[0] - warped_pos[0]));
    delta[1] = max_ff(delta[1], fabsf(pos[1] - warped_pos[1]));

    if (a >= rect->xmax) {
      break;
    }
  }

  for (int a = rect->ymin; a <= rect->ymax + coord_delta; a += coord_delta) {
    if (a > rect->ymax) {
      a = rect->ymax;
    }

    /* left edge */
    pos[0] = rect->xmin;
    pos[1] = a;

    apply_distortion(tracking, image_width, image_height, pos, warped_pos);

    delta[0] = max_ff(delta[0], fabsf(pos[0] - warped_pos[0]));
    delta[1] = max_ff(delta[1], fabsf(pos[1] - warped_pos[1]));

    /* right edge */
    pos[0] = rect->xmax;
    pos[1] = a;

    apply_distortion(tracking, image_width, image_height, pos, warped_pos);

    delta[0] = max_ff(delta[0], fabsf(pos[0] - warped_pos[0]));
    delta[1] = max_ff(delta[1], fabsf(pos[1] - warped_pos[1]));

    if (a >= rect->ymax) {
      break;
    }
  }
}

/* --------------------------------------------------------------------
 * Image sampling.
 */

static void disable_imbuf_channels(ImBuf *ibuf, MovieTrackingTrack *track, bool grayscale)
{
  BKE_tracking_disable_channels(ibuf,
                                track->flag & TRACK_DISABLE_RED,
                                track->flag & TRACK_DISABLE_GREEN,
                                track->flag & TRACK_DISABLE_BLUE,
                                grayscale);
}

ImBuf *BKE_tracking_sample_pattern(int frame_width,
                                   int frame_height,
                                   ImBuf *search_ibuf,
                                   MovieTrackingTrack *track,
                                   MovieTrackingMarker *marker,
                                   bool from_anchor,
                                   bool use_mask,
                                   int num_samples_x,
                                   int num_samples_y,
                                   float pos[2])
{
  ImBuf *pattern_ibuf;
  double src_pixel_x[5], src_pixel_y[5];
  double warped_position_x, warped_position_y;
  float *mask = NULL;

  if (num_samples_x <= 0 || num_samples_y <= 0) {
    return NULL;
  }

  pattern_ibuf = IMB_allocImBuf(
      num_samples_x, num_samples_y, 32, search_ibuf->rect_float ? IB_rectfloat : IB_rect);

  tracking_get_marker_coords_for_tracking(
      frame_width, frame_height, marker, src_pixel_x, src_pixel_y);

  /* from_anchor means search buffer was obtained for an anchored position,
   * which means applying track offset rounded to pixel space (we could not
   * store search buffer with sub-pixel precision)
   *
   * in this case we need to alter coordinates a bit, to compensate rounded
   * fractional part of offset
   */
  if (from_anchor) {
    for (int a = 0; a < 5; a++) {
      src_pixel_x[a] += (double)((track->offset[0] * frame_width) -
                                 ((int)(track->offset[0] * frame_width)));
      src_pixel_y[a] += (double)((track->offset[1] * frame_height) -
                                 ((int)(track->offset[1] * frame_height)));

      /* when offset is negative, rounding happens in opposite direction */
      if (track->offset[0] < 0.0f) {
        src_pixel_x[a] += 1.0;
      }
      if (track->offset[1] < 0.0f) {
        src_pixel_y[a] += 1.0;
      }
    }
  }

  if (use_mask) {
    mask = BKE_tracking_track_get_mask(frame_width, frame_height, track, marker);
  }

  if (search_ibuf->rect_float) {
    libmv_samplePlanarPatchFloat(search_ibuf->rect_float,
                                 search_ibuf->x,
                                 search_ibuf->y,
                                 4,
                                 src_pixel_x,
                                 src_pixel_y,
                                 num_samples_x,
                                 num_samples_y,
                                 mask,
                                 pattern_ibuf->rect_float,
                                 &warped_position_x,
                                 &warped_position_y);
  }
  else {
    libmv_samplePlanarPatchByte((uchar *)search_ibuf->rect,
                                search_ibuf->x,
                                search_ibuf->y,
                                4,
                                src_pixel_x,
                                src_pixel_y,
                                num_samples_x,
                                num_samples_y,
                                mask,
                                (uchar *)pattern_ibuf->rect,
                                &warped_position_x,
                                &warped_position_y);
  }

  if (pos) {
    pos[0] = warped_position_x;
    pos[1] = warped_position_y;
  }

  if (mask) {
    MEM_freeN(mask);
  }

  return pattern_ibuf;
}

ImBuf *BKE_tracking_get_pattern_imbuf(ImBuf *ibuf,
                                      MovieTrackingTrack *track,
                                      MovieTrackingMarker *marker,
                                      bool anchored,
                                      bool disable_channels)
{
  ImBuf *pattern_ibuf, *search_ibuf;
  float pat_min[2], pat_max[2];
  int num_samples_x, num_samples_y;

  BKE_tracking_marker_pattern_minmax(marker, pat_min, pat_max);

  num_samples_x = (pat_max[0] - pat_min[0]) * ibuf->x;
  num_samples_y = (pat_max[1] - pat_min[1]) * ibuf->y;

  search_ibuf = BKE_tracking_get_search_imbuf(ibuf, track, marker, anchored, disable_channels);

  if (search_ibuf) {
    pattern_ibuf = BKE_tracking_sample_pattern(ibuf->x,
                                               ibuf->y,
                                               search_ibuf,
                                               track,
                                               marker,
                                               anchored,
                                               false,
                                               num_samples_x,
                                               num_samples_y,
                                               NULL);

    IMB_freeImBuf(search_ibuf);
  }
  else {
    pattern_ibuf = NULL;
  }

  return pattern_ibuf;
}

ImBuf *BKE_tracking_get_search_imbuf(ImBuf *ibuf,
                                     MovieTrackingTrack *track,
                                     MovieTrackingMarker *marker,
                                     bool anchored,
                                     bool disable_channels)
{
  ImBuf *searchibuf;
  int x, y, w, h;
  float search_origin[2];

  tracking_get_search_origin_frame_pixel(ibuf->x, ibuf->y, marker, search_origin);

  x = search_origin[0];
  y = search_origin[1];

  if (anchored) {
    x += track->offset[0] * ibuf->x;
    y += track->offset[1] * ibuf->y;
  }

  w = (marker->search_max[0] - marker->search_min[0]) * ibuf->x;
  h = (marker->search_max[1] - marker->search_min[1]) * ibuf->y;

  if (w <= 0 || h <= 0) {
    return NULL;
  }

  searchibuf = IMB_allocImBuf(w, h, 32, ibuf->rect_float ? IB_rectfloat : IB_rect);

  IMB_rectcpy(searchibuf, ibuf, 0, 0, x, y, w, h);

  if (disable_channels) {
    if ((track->flag & TRACK_PREVIEW_GRAYSCALE) || (track->flag & TRACK_DISABLE_RED) ||
        (track->flag & TRACK_DISABLE_GREEN) || (track->flag & TRACK_DISABLE_BLUE)) {
      disable_imbuf_channels(searchibuf, track, true);
    }
  }

  return searchibuf;
}

BLI_INLINE int plane_marker_size_len_in_pixels(const float a[2],
                                               const float b[2],
                                               const int frame_width,
                                               const int frame_height)
{
  const float a_px[2] = {a[0] * frame_width, a[1] * frame_height};
  const float b_px[2] = {b[0] * frame_width, b[1] * frame_height};

  return ceilf(len_v2v2(a_px, b_px));
}

ImBuf *BKE_tracking_get_plane_imbuf(const ImBuf *frame_ibuf,
                                    const MovieTrackingPlaneMarker *plane_marker)
{
  /* Alias for corners, allowing shorter access to coordinates. */
  const float(*corners)[2] = plane_marker->corners;

  /* Dimensions of the frame image in pixels. */
  const int frame_width = frame_ibuf->x;
  const int frame_height = frame_ibuf->y;

  /* Lengths of left and right edges of the plane marker, in pixels. */
  const int left_side_len_px = plane_marker_size_len_in_pixels(
      corners[0], corners[3], frame_width, frame_height);
  const int right_side_len_px = plane_marker_size_len_in_pixels(
      corners[1], corners[2], frame_width, frame_height);

  /* Lengths of top and bottom edges of the plane marker, in pixels. */
  const int top_side_len_px = plane_marker_size_len_in_pixels(
      corners[3], corners[2], frame_width, frame_height);
  const int bottom_side_len_px = plane_marker_size_len_in_pixels(
      corners[0], corners[1], frame_width, frame_height);

  /* Choose the number of samples as a maximum of the corresponding sides in pixels. */
  const int num_samples_x = max_ii(top_side_len_px, bottom_side_len_px);
  const int num_samples_y = max_ii(left_side_len_px, right_side_len_px);

  /* Create new result image with the same type of content as the original. */
  ImBuf *plane_ibuf = IMB_allocImBuf(
      num_samples_x, num_samples_y, 32, frame_ibuf->rect_float ? IB_rectfloat : IB_rect);

  /* Calculate corner coordinates in pixel space, as separate X/Y arrays. */
  const double src_pixel_x[4] = {corners[0][0] * frame_width,
                                 corners[1][0] * frame_width,
                                 corners[2][0] * frame_width,
                                 corners[3][0] * frame_width};
  const double src_pixel_y[4] = {corners[0][1] * frame_height,
                                 corners[1][1] * frame_height,
                                 corners[2][1] * frame_height,
                                 corners[3][1] * frame_height};

  /* Warped Position is unused but is expected to be provided by the API. */
  double warped_position_x, warped_position_y;

  /* Actual sampling. */
  if (frame_ibuf->rect_float != NULL) {
    libmv_samplePlanarPatchFloat(frame_ibuf->rect_float,
                                 frame_ibuf->x,
                                 frame_ibuf->y,
                                 4,
                                 src_pixel_x,
                                 src_pixel_y,
                                 num_samples_x,
                                 num_samples_y,
                                 NULL,
                                 plane_ibuf->rect_float,
                                 &warped_position_x,
                                 &warped_position_y);
  }
  else {
    libmv_samplePlanarPatchByte((uchar *)frame_ibuf->rect,
                                frame_ibuf->x,
                                frame_ibuf->y,
                                4,
                                src_pixel_x,
                                src_pixel_y,
                                num_samples_x,
                                num_samples_y,
                                NULL,
                                (uchar *)plane_ibuf->rect,
                                &warped_position_x,
                                &warped_position_y);
  }

  plane_ibuf->rect_colorspace = frame_ibuf->rect_colorspace;
  plane_ibuf->float_colorspace = frame_ibuf->float_colorspace;

  return plane_ibuf;
}

void BKE_tracking_disable_channels(
    ImBuf *ibuf, bool disable_red, bool disable_green, bool disable_blue, bool grayscale)
{
  if (!disable_red && !disable_green && !disable_blue && !grayscale) {
    return;
  }

  /* if only some components are selected, it's important to rescale the result
   * appropriately so that e.g. if only blue is selected, it's not zeroed out.
   */
  float scale = (disable_red ? 0.0f : 0.2126f) + (disable_green ? 0.0f : 0.7152f) +
                (disable_blue ? 0.0f : 0.0722f);

  for (int y = 0; y < ibuf->y; y++) {
    for (int x = 0; x < ibuf->x; x++) {
      int pixel = ibuf->x * y + x;

      if (ibuf->rect_float) {
        float *rrgbf = ibuf->rect_float + pixel * 4;
        float r = disable_red ? 0.0f : rrgbf[0];
        float g = disable_green ? 0.0f : rrgbf[1];
        float b = disable_blue ? 0.0f : rrgbf[2];

        if (grayscale) {
          float gray = (0.2126f * r + 0.7152f * g + 0.0722f * b) / scale;

          rrgbf[0] = rrgbf[1] = rrgbf[2] = gray;
        }
        else {
          rrgbf[0] = r;
          rrgbf[1] = g;
          rrgbf[2] = b;
        }
      }
      else {
        char *rrgb = (char *)ibuf->rect + pixel * 4;
        char r = disable_red ? 0 : rrgb[0];
        char g = disable_green ? 0 : rrgb[1];
        char b = disable_blue ? 0 : rrgb[2];

        if (grayscale) {
          float gray = (0.2126f * r + 0.7152f * g + 0.0722f * b) / scale;

          rrgb[0] = rrgb[1] = rrgb[2] = gray;
        }
        else {
          rrgb[0] = r;
          rrgb[1] = g;
          rrgb[2] = b;
        }
      }
    }
  }

  if (ibuf->rect_float) {
    ibuf->userflags |= IB_RECT_INVALID;
  }
}

/* --------------------------------------------------------------------
 * Dopesheet functions.
 */

/* ** Channels sort comparators ** */

static int channels_alpha_sort(const void *a, const void *b)
{
  const MovieTrackingDopesheetChannel *channel_a = a;
  const MovieTrackingDopesheetChannel *channel_b = b;

  if (BLI_strcasecmp(channel_a->track->name, channel_b->track->name) > 0) {
    return 1;
  }

  return 0;
}

static int channels_total_track_sort(const void *a, const void *b)
{
  const MovieTrackingDopesheetChannel *channel_a = a;
  const MovieTrackingDopesheetChannel *channel_b = b;

  if (channel_a->total_frames > channel_b->total_frames) {
    return 1;
  }

  return 0;
}

static int channels_longest_segment_sort(const void *a, const void *b)
{
  const MovieTrackingDopesheetChannel *channel_a = a;
  const MovieTrackingDopesheetChannel *channel_b = b;

  if (channel_a->max_segment > channel_b->max_segment) {
    return 1;
  }

  return 0;
}

static int channels_average_error_sort(const void *a, const void *b)
{
  const MovieTrackingDopesheetChannel *channel_a = a;
  const MovieTrackingDopesheetChannel *channel_b = b;

  if (channel_a->track->error > channel_b->track->error) {
    return 1;
  }

  if (channel_a->track->error == channel_b->track->error) {
    return channels_alpha_sort(a, b);
  }

  return 0;
}

static int compare_firstlast_putting_undefined_first(
    bool inverse, bool a_markerless, int a_value, bool b_markerless, int b_value)
{
  if (a_markerless && b_markerless) {
    /* Neither channel has not-disabled markers, return whatever. */
    return 0;
  }
  if (a_markerless) {
    /* Put the markerless channel first. */
    return 0;
  }
  if (b_markerless) {
    /* Put the markerless channel first. */
    return 1;
  }

  /* Both channels have markers. */

  if (inverse) {
    if (a_value < b_value) {
      return 1;
    }
    return 0;
  }

  if (a_value > b_value) {
    return 1;
  }
  return 0;
}

static int channels_start_sort(const void *a, const void *b)
{
  const MovieTrackingDopesheetChannel *channel_a = a;
  const MovieTrackingDopesheetChannel *channel_b = b;

  return compare_firstlast_putting_undefined_first(false,
                                                   channel_a->tot_segment == 0,
                                                   channel_a->first_not_disabled_marker_framenr,
                                                   channel_b->tot_segment == 0,
                                                   channel_b->first_not_disabled_marker_framenr);
}

static int channels_end_sort(const void *a, const void *b)
{
  const MovieTrackingDopesheetChannel *channel_a = a;
  const MovieTrackingDopesheetChannel *channel_b = b;

  return compare_firstlast_putting_undefined_first(false,
                                                   channel_a->tot_segment == 0,
                                                   channel_a->last_not_disabled_marker_framenr,
                                                   channel_b->tot_segment == 0,
                                                   channel_b->last_not_disabled_marker_framenr);
}

static int channels_alpha_inverse_sort(const void *a, const void *b)
{
  if (channels_alpha_sort(a, b)) {
    return 0;
  }

  return 1;
}

static int channels_total_track_inverse_sort(const void *a, const void *b)
{
  if (channels_total_track_sort(a, b)) {
    return 0;
  }

  return 1;
}

static int channels_longest_segment_inverse_sort(const void *a, const void *b)
{
  if (channels_longest_segment_sort(a, b)) {
    return 0;
  }

  return 1;
}

static int channels_average_error_inverse_sort(const void *a, const void *b)
{
  const MovieTrackingDopesheetChannel *channel_a = a;
  const MovieTrackingDopesheetChannel *channel_b = b;

  if (channel_a->track->error < channel_b->track->error) {
    return 1;
  }

  return 0;
}

static int channels_start_inverse_sort(const void *a, const void *b)
{
  const MovieTrackingDopesheetChannel *channel_a = a;
  const MovieTrackingDopesheetChannel *channel_b = b;

  return compare_firstlast_putting_undefined_first(true,
                                                   channel_a->tot_segment == 0,
                                                   channel_a->first_not_disabled_marker_framenr,
                                                   channel_b->tot_segment == 0,
                                                   channel_b->first_not_disabled_marker_framenr);
}

static int channels_end_inverse_sort(const void *a, const void *b)
{
  const MovieTrackingDopesheetChannel *channel_a = a;
  const MovieTrackingDopesheetChannel *channel_b = b;

  return compare_firstlast_putting_undefined_first(true,
                                                   channel_a->tot_segment == 0,
                                                   channel_a->last_not_disabled_marker_framenr,
                                                   channel_b->tot_segment == 0,
                                                   channel_b->last_not_disabled_marker_framenr);
}

/* Calculate frames segments at which track is tracked continuously. */
static void tracking_dopesheet_channels_segments_calc(MovieTrackingDopesheetChannel *channel)
{
  MovieTrackingTrack *track = channel->track;
  int i, segment;
  bool first_not_disabled_marker_framenr_set;

  channel->tot_segment = 0;
  channel->max_segment = 0;
  channel->total_frames = 0;

  channel->first_not_disabled_marker_framenr = 0;
  channel->last_not_disabled_marker_framenr = 0;

  /* TODO(sergey): looks a bit code-duplicated, need to look into
   *               logic de-duplication here.
   */

  /* count */
  i = 0;
  first_not_disabled_marker_framenr_set = false;
  while (i < track->markersnr) {
    MovieTrackingMarker *marker = &track->markers[i];

    if ((marker->flag & MARKER_DISABLED) == 0) {
      int prev_fra = marker->framenr, len = 0;

      i++;
      while (i < track->markersnr) {
        marker = &track->markers[i];

        if (marker->framenr != prev_fra + 1) {
          break;
        }
        if (marker->flag & MARKER_DISABLED) {
          break;
        }

        if (!first_not_disabled_marker_framenr_set) {
          channel->first_not_disabled_marker_framenr = marker->framenr;
          first_not_disabled_marker_framenr_set = true;
        }
        channel->last_not_disabled_marker_framenr = marker->framenr;

        prev_fra = marker->framenr;
        len++;
        i++;
      }

      channel->tot_segment++;
    }

    i++;
  }

  if (!channel->tot_segment) {
    return;
  }

  channel->segments = MEM_callocN(sizeof(int[2]) * channel->tot_segment,
                                  "tracking channel segments");

  /* create segments */
  i = 0;
  segment = 0;
  while (i < track->markersnr) {
    MovieTrackingMarker *marker = &track->markers[i];

    if ((marker->flag & MARKER_DISABLED) == 0) {
      MovieTrackingMarker *start_marker = marker;
      int prev_fra = marker->framenr, len = 0;

      i++;
      while (i < track->markersnr) {
        marker = &track->markers[i];

        if (marker->framenr != prev_fra + 1) {
          break;
        }
        if (marker->flag & MARKER_DISABLED) {
          break;
        }

        prev_fra = marker->framenr;
        channel->total_frames++;
        len++;
        i++;
      }

      channel->segments[2 * segment] = start_marker->framenr;
      channel->segments[2 * segment + 1] = start_marker->framenr + len;

      channel->max_segment = max_ii(channel->max_segment, len);
      segment++;
    }

    i++;
  }
}

/* Create channels for tracks and calculate tracked segments for them. */
static void tracking_dopesheet_channels_calc(MovieTracking *tracking)
{
  MovieTrackingObject *object = BKE_tracking_object_get_active(tracking);
  MovieTrackingDopesheet *dopesheet = &tracking->dopesheet;
  MovieTrackingReconstruction *reconstruction = BKE_tracking_object_get_reconstruction(tracking,
                                                                                       object);
  ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);

  bool sel_only = (dopesheet->flag & TRACKING_DOPE_SELECTED_ONLY) != 0;
  bool show_hidden = (dopesheet->flag & TRACKING_DOPE_SHOW_HIDDEN) != 0;

  LISTBASE_FOREACH (MovieTrackingTrack *, track, tracksbase) {
    if (!show_hidden && (track->flag & TRACK_HIDDEN) != 0) {
      continue;
    }

    if (sel_only && !TRACK_SELECTED(track)) {
      continue;
    }

    MovieTrackingDopesheetChannel *channel = MEM_callocN(sizeof(MovieTrackingDopesheetChannel),
                                                         "tracking dopesheet channel");
    channel->track = track;

    if (reconstruction->flag & TRACKING_RECONSTRUCTED) {
      BLI_snprintf(channel->name, sizeof(channel->name), "%s (%.4f)", track->name, track->error);
    }
    else {
      BLI_strncpy(channel->name, track->name, sizeof(channel->name));
    }

    tracking_dopesheet_channels_segments_calc(channel);

    BLI_addtail(&dopesheet->channels, channel);
    dopesheet->tot_channel++;
  }
}

/* Sot dopesheet channels using given method (name, average error, total coverage,
 * longest tracked segment) and could also inverse the list if it's enabled.
 */
static void tracking_dopesheet_channels_sort(MovieTracking *tracking,
                                             int sort_method,
                                             bool inverse)
{
  MovieTrackingDopesheet *dopesheet = &tracking->dopesheet;

  if (inverse) {
    if (sort_method == TRACKING_DOPE_SORT_NAME) {
      BLI_listbase_sort(&dopesheet->channels, channels_alpha_inverse_sort);
    }
    else if (sort_method == TRACKING_DOPE_SORT_LONGEST) {
      BLI_listbase_sort(&dopesheet->channels, channels_longest_segment_inverse_sort);
    }
    else if (sort_method == TRACKING_DOPE_SORT_TOTAL) {
      BLI_listbase_sort(&dopesheet->channels, channels_total_track_inverse_sort);
    }
    else if (sort_method == TRACKING_DOPE_SORT_AVERAGE_ERROR) {
      BLI_listbase_sort(&dopesheet->channels, channels_average_error_inverse_sort);
    }
    else if (sort_method == TRACKING_DOPE_SORT_START) {
      BLI_listbase_sort(&dopesheet->channels, channels_start_inverse_sort);
    }
    else if (sort_method == TRACKING_DOPE_SORT_END) {
      BLI_listbase_sort(&dopesheet->channels, channels_end_inverse_sort);
    }
  }
  else {
    if (sort_method == TRACKING_DOPE_SORT_NAME) {
      BLI_listbase_sort(&dopesheet->channels, channels_alpha_sort);
    }
    else if (sort_method == TRACKING_DOPE_SORT_LONGEST) {
      BLI_listbase_sort(&dopesheet->channels, channels_longest_segment_sort);
    }
    else if (sort_method == TRACKING_DOPE_SORT_TOTAL) {
      BLI_listbase_sort(&dopesheet->channels, channels_total_track_sort);
    }
    else if (sort_method == TRACKING_DOPE_SORT_AVERAGE_ERROR) {
      BLI_listbase_sort(&dopesheet->channels, channels_average_error_sort);
    }
    else if (sort_method == TRACKING_DOPE_SORT_START) {
      BLI_listbase_sort(&dopesheet->channels, channels_start_sort);
    }
    else if (sort_method == TRACKING_DOPE_SORT_END) {
      BLI_listbase_sort(&dopesheet->channels, channels_end_sort);
    }
  }
}

static int coverage_from_count(int count)
{
  /* Values are actually arbitrary here, probably need to be tweaked. */
  if (count < 8) {
    return TRACKING_COVERAGE_BAD;
  }
  if (count < 16) {
    return TRACKING_COVERAGE_ACCEPTABLE;
  }
  return TRACKING_COVERAGE_OK;
}

/* Calculate coverage of frames with tracks, this information
 * is used to highlight dopesheet background depending on how
 * many tracks exists on the frame.
 */
static void tracking_dopesheet_calc_coverage(MovieTracking *tracking)
{
  MovieTrackingDopesheet *dopesheet = &tracking->dopesheet;
  MovieTrackingObject *object = BKE_tracking_object_get_active(tracking);
  ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);
  int frames, start_frame = INT_MAX, end_frame = -INT_MAX;
  int *per_frame_counter;
  int prev_coverage, last_segment_frame;

  /* find frame boundaries */
  LISTBASE_FOREACH (MovieTrackingTrack *, track, tracksbase) {
    start_frame = min_ii(start_frame, track->markers[0].framenr);
    end_frame = max_ii(end_frame, track->markers[track->markersnr - 1].framenr);
  }

  if (start_frame > end_frame) {
    /* There are no markers at all, nothing to calculate coverage from. */
    return;
  }

  frames = end_frame - start_frame + 1;

  /* this is a per-frame counter of markers (how many markers belongs to the same frame) */
  per_frame_counter = MEM_callocN(sizeof(int) * frames, "per frame track counter");

  /* find per-frame markers count */
  LISTBASE_FOREACH (MovieTrackingTrack *, track, tracksbase) {
    for (int i = 0; i < track->markersnr; i++) {
      MovieTrackingMarker *marker = &track->markers[i];

      /* TODO: perhaps we need to add check for non-single-frame track here */
      if ((marker->flag & MARKER_DISABLED) == 0) {
        per_frame_counter[marker->framenr - start_frame]++;
      }
    }
  }

  /* convert markers count to coverage and detect segments with the same coverage */
  prev_coverage = coverage_from_count(per_frame_counter[0]);
  last_segment_frame = start_frame;

  /* means only disabled tracks in the beginning, could be ignored */
  if (!per_frame_counter[0]) {
    prev_coverage = TRACKING_COVERAGE_OK;
  }

  for (int i = 1; i < frames; i++) {
    int coverage = coverage_from_count(per_frame_counter[i]);

    /* means only disabled tracks in the end, could be ignored */
    if (i == frames - 1 && !per_frame_counter[i]) {
      coverage = TRACKING_COVERAGE_OK;
    }

    if (coverage != prev_coverage || i == frames - 1) {
      MovieTrackingDopesheetCoverageSegment *coverage_segment;
      int end_segment_frame = i - 1 + start_frame;

      if (end_segment_frame == last_segment_frame) {
        end_segment_frame++;
      }

      coverage_segment = MEM_callocN(sizeof(MovieTrackingDopesheetCoverageSegment),
                                     "tracking coverage segment");
      coverage_segment->coverage = prev_coverage;
      coverage_segment->start_frame = last_segment_frame;
      coverage_segment->end_frame = end_segment_frame;

      BLI_addtail(&dopesheet->coverage_segments, coverage_segment);

      last_segment_frame = end_segment_frame;
    }

    prev_coverage = coverage;
  }

  MEM_freeN(per_frame_counter);
}

void BKE_tracking_dopesheet_tag_update(MovieTracking *tracking)
{
  MovieTrackingDopesheet *dopesheet = &tracking->dopesheet;

  dopesheet->ok = false;
}

void BKE_tracking_dopesheet_update(MovieTracking *tracking)
{
  MovieTrackingDopesheet *dopesheet = &tracking->dopesheet;

  short sort_method = dopesheet->sort_method;
  bool inverse = (dopesheet->flag & TRACKING_DOPE_SORT_INVERSE) != 0;

  if (dopesheet->ok) {
    return;
  }

  tracking_dopesheet_free(dopesheet);

  /* channels */
  tracking_dopesheet_channels_calc(tracking);
  tracking_dopesheet_channels_sort(tracking, sort_method, inverse);

  /* frame coverage */
  tracking_dopesheet_calc_coverage(tracking);

  dopesheet->ok = true;
}

MovieTrackingObject *BKE_tracking_find_object_for_track(const MovieTracking *tracking,
                                                        const MovieTrackingTrack *track)
{
  const ListBase *tracksbase = &tracking->tracks;
  if (BLI_findindex(tracksbase, track) != -1) {
    return NULL;
  }
  MovieTrackingObject *object = tracking->objects.first;
  while (object != NULL) {
    if (BLI_findindex(&object->tracks, track) != -1) {
      return object;
    }
    object = object->next;
  }
  return NULL;
}

ListBase *BKE_tracking_find_tracks_list_for_track(MovieTracking *tracking,
                                                  const MovieTrackingTrack *track)
{
  MovieTrackingObject *object = BKE_tracking_find_object_for_track(tracking, track);
  if (object != NULL) {
    return &object->tracks;
  }
  return &tracking->tracks;
}

MovieTrackingObject *BKE_tracking_find_object_for_plane_track(
    const MovieTracking *tracking, const MovieTrackingPlaneTrack *plane_track)
{
  const ListBase *plane_tracks_base = &tracking->plane_tracks;
  if (BLI_findindex(plane_tracks_base, plane_track) != -1) {
    return NULL;
  }
  MovieTrackingObject *object = tracking->objects.first;
  while (object != NULL) {
    if (BLI_findindex(&object->plane_tracks, plane_track) != -1) {
      return object;
    }
    object = object->next;
  }
  return NULL;
}

ListBase *BKE_tracking_find_tracks_list_for_plane_track(MovieTracking *tracking,
                                                        const MovieTrackingPlaneTrack *plane_track)
{
  MovieTrackingObject *object = BKE_tracking_find_object_for_plane_track(tracking, plane_track);
  if (object != NULL) {
    return &object->plane_tracks;
  }
  return &tracking->plane_tracks;
}

void BKE_tracking_get_rna_path_for_track(const struct MovieTracking *tracking,
                                         const struct MovieTrackingTrack *track,
                                         char *rna_path,
                                         size_t rna_path_len)
{
  MovieTrackingObject *object = BKE_tracking_find_object_for_track(tracking, track);
  char track_name_esc[MAX_NAME * 2];
  BLI_str_escape(track_name_esc, track->name, sizeof(track_name_esc));
  if (object == NULL) {
    BLI_snprintf(rna_path, rna_path_len, "tracking.tracks[\"%s\"]", track_name_esc);
  }
  else {
    char object_name_esc[MAX_NAME * 2];
    BLI_str_escape(object_name_esc, object->name, sizeof(object_name_esc));
    BLI_snprintf(rna_path,
                 rna_path_len,
                 "tracking.objects[\"%s\"].tracks[\"%s\"]",
                 object_name_esc,
                 track_name_esc);
  }
}

void BKE_tracking_get_rna_path_prefix_for_track(const struct MovieTracking *tracking,
                                                const struct MovieTrackingTrack *track,
                                                char *rna_path,
                                                size_t rna_path_len)
{
  MovieTrackingObject *object = BKE_tracking_find_object_for_track(tracking, track);
  if (object == NULL) {
    BLI_strncpy(rna_path, "tracking.tracks", rna_path_len);
  }
  else {
    char object_name_esc[MAX_NAME * 2];
    BLI_str_escape(object_name_esc, object->name, sizeof(object_name_esc));
    BLI_snprintf(rna_path, rna_path_len, "tracking.objects[\"%s\"]", object_name_esc);
  }
}

void BKE_tracking_get_rna_path_for_plane_track(const struct MovieTracking *tracking,
                                               const struct MovieTrackingPlaneTrack *plane_track,
                                               char *rna_path,
                                               size_t rna_path_len)
{
  MovieTrackingObject *object = BKE_tracking_find_object_for_plane_track(tracking, plane_track);
  char track_name_esc[MAX_NAME * 2];
  BLI_str_escape(track_name_esc, plane_track->name, sizeof(track_name_esc));
  if (object == NULL) {
    BLI_snprintf(rna_path, rna_path_len, "tracking.plane_tracks[\"%s\"]", track_name_esc);
  }
  else {
    char object_name_esc[MAX_NAME * 2];
    BLI_str_escape(object_name_esc, object->name, sizeof(object_name_esc));
    BLI_snprintf(rna_path,
                 rna_path_len,
                 "tracking.objects[\"%s\"].plane_tracks[\"%s\"]",
                 object_name_esc,
                 track_name_esc);
  }
}

void BKE_tracking_get_rna_path_prefix_for_plane_track(
    const struct MovieTracking *tracking,
    const struct MovieTrackingPlaneTrack *plane_track,
    char *rna_path,
    size_t rna_path_len)
{
  MovieTrackingObject *object = BKE_tracking_find_object_for_plane_track(tracking, plane_track);
  if (object == NULL) {
    BLI_strncpy(rna_path, "tracking.plane_tracks", rna_path_len);
  }
  else {
    char object_name_esc[MAX_NAME * 2];
    BLI_str_escape(object_name_esc, object->name, sizeof(object_name_esc));
    BLI_snprintf(rna_path, rna_path_len, "tracking.objects[\"%s\"].plane_tracks", object_name_esc);
  }
}

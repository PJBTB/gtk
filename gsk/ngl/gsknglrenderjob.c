/* gsknglrenderjob.c
 *
 * Copyright 2017 Timm Bäder <mail@baedert.org>
 * Copyright 2018 Matthias Clasen <mclasen@redhat.com>
 * Copyright 2018 Alexander Larsson <alexl@redhat.com>
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <gdk/gdkglcontextprivate.h>
#include <gdk/gdkprofilerprivate.h>
#include <gdk/gdkrgbaprivate.h>
#include <gsk/gskrendernodeprivate.h>
#include <gsk/gskglshaderprivate.h>
#include <gdk/gdktextureprivate.h>
#include <gsk/gsktransformprivate.h>
#include <gsk/gskroundedrectprivate.h>
#include <math.h>
#include <string.h>

#include "gsknglcommandqueueprivate.h"
#include "gskngldriverprivate.h"
#include "gsknglglyphlibraryprivate.h"
#include "gskngliconlibraryprivate.h"
#include "gsknglprogramprivate.h"
#include "gsknglrenderjobprivate.h"
#include "gsknglshadowlibraryprivate.h"

#include "ninesliceprivate.h"

#define ORTHO_NEAR_PLANE   -10000
#define ORTHO_FAR_PLANE     10000
#define MAX_GRADIENT_STOPS  6
#define SHADOW_EXTRA_SIZE   4

/* Make sure gradient stops fits in packed array_count */
G_STATIC_ASSERT ((MAX_GRADIENT_STOPS * 5) < (1 << GSK_NGL_UNIFORM_ARRAY_BITS));

#define rounded_rect_top_left(r)                                                        \
  (GRAPHENE_RECT_INIT(r->bounds.origin.x,                                               \
                      r->bounds.origin.y,                                               \
                      r->corner[0].width, r->corner[0].height))
#define rounded_rect_top_right(r) \
  (GRAPHENE_RECT_INIT(r->bounds.origin.x + r->bounds.size.width - r->corner[1].width,   \
                      r->bounds.origin.y, \
                      r->corner[1].width, r->corner[1].height))
#define rounded_rect_bottom_right(r) \
  (GRAPHENE_RECT_INIT(r->bounds.origin.x + r->bounds.size.width - r->corner[2].width,   \
                      r->bounds.origin.y + r->bounds.size.height - r->corner[2].height, \
                      r->corner[2].width, r->corner[2].height))
#define rounded_rect_bottom_left(r)                                                     \
  (GRAPHENE_RECT_INIT(r->bounds.origin.x,                                               \
                      r->bounds.origin.y + r->bounds.size.height - r->corner[2].height, \
                      r->corner[3].width, r->corner[3].height))
#define rounded_rect_corner0(r)   rounded_rect_top_left(r)
#define rounded_rect_corner1(r)   rounded_rect_top_right(r)
#define rounded_rect_corner2(r)   rounded_rect_bottom_right(r)
#define rounded_rect_corner3(r)   rounded_rect_bottom_left(r)
#define rounded_rect_corner(r, i) (rounded_rect_corner##i(r))
#define ALPHA_IS_CLEAR(alpha) ((alpha) < ((float) 0x00ff / (float) 0xffff))
#define RGBA_IS_CLEAR(rgba) ALPHA_IS_CLEAR((rgba)->alpha)

typedef struct _GskNglRenderClip
{
  GskRoundedRect rect;
  guint          is_rectilinear : 1;
  guint          is_fully_contained : 1;
} GskNglRenderClip;

typedef struct _GskNglRenderModelview
{
  GskTransform *transform;
  float scale_x;
  float scale_y;
  float offset_x_before;
  float offset_y_before;
  graphene_matrix_t matrix;
} GskNglRenderModelview;

struct _GskNglRenderJob
{
  /* The context containing the framebuffer we are drawing to. Generally this
   * is the context of the surface but may be a shared context if rendering to
   * an offscreen texture such as gsk_ngl_renderer_render_texture().
   */
  GdkGLContext *context;

  /* The driver to be used. This is shared among all the renderers on a given
   * GdkDisplay and uses the shared GL context to send commands.
   */
  GskNglDriver *driver;

  /* The command queue (which is just a faster pointer to the driver's
   * command queue.
   */
  GskNglCommandQueue *command_queue;

  /* The region that we are clipping. Normalized to a single rectangle region. */
  cairo_region_t *region;

  /* The framebuffer to draw to in the @context GL context. So 0 would be the
   * default framebuffer of @context. This is important to note as many other
   * operations could be done using objects shared from the command queues
   * GL context.
   */
  guint framebuffer;

  /* The viewport we are using. This state is updated as we process render
   * nodes in the specific visitor callbacks.
   */
  graphene_rect_t viewport;

  /* The current projection, updated as we process nodes */
  graphene_matrix_t projection;

  /* An array of GskNglRenderModelview updated as nodes are processed. The
   * current modelview is the last element.
   */
  GArray *modelview;

  /* An array of GskNglRenderClip updated as nodes are processed. The
   * current clip is the last element.
   */
  GArray *clip;

  /* Our current alpha state as we process nodes */
  float alpha;

  /* Our current color state as we process nodes */
  GdkRGBA color;

  /* Offset (delta x,y) as we process nodes. Occasionally this is merged into
   * a transform that is referenced from child transform nodes.
   */
  float offset_x;
  float offset_y;

  /* The scale we are processing, possibly updated by transforms */
  float scale_x;
  float scale_y;

  /* Cached pointers */
  const GskNglRenderClip *current_clip;
  const GskNglRenderModelview *current_modelview;
  GskNglProgram *current_program;

  /* If we should be rendering red zones over fallback nodes */
  guint debug_fallback : 1;
};

typedef struct _GskNglRenderOffscreen
{
  const graphene_rect_t *bounds;
  struct {
    float x;
    float y;
    float x2;
    float y2;
  } area;
  guint texture_id;
  guint force_offscreen : 1;
  guint reset_clip : 1;
  guint do_not_cache : 1;
  guint linear_filter : 1;
  guint was_offscreen : 1;
} GskNglRenderOffscreen;

static void     gsk_ngl_render_job_visit_node                (GskNglRenderJob       *job,
                                                              const GskRenderNode  *node);
static gboolean gsk_ngl_render_job_visit_node_with_offscreen (GskNglRenderJob       *job,
                                                              const GskRenderNode  *node,
                                                              GskNglRenderOffscreen *offscreen);

static inline void
init_full_texture_region (GskNglRenderOffscreen *offscreen)
{
  offscreen->area.x = 0;
  offscreen->area.y = 0;
  offscreen->area.x2 = 1;
  offscreen->area.y2 = 1;
}

static inline gboolean G_GNUC_PURE
node_is_invisible (const GskRenderNode *node)
{
  return node->bounds.size.width == 0.0f ||
         node->bounds.size.height == 0.0f;
}

static inline void
gsk_rounded_rect_shrink_to_minimum (GskRoundedRect *self)
{
  self->bounds.size.width  = MAX (self->corner[0].width + self->corner[1].width,
                                  self->corner[3].width + self->corner[2].width);
  self->bounds.size.height = MAX (self->corner[0].height + self->corner[3].height,
                                  self->corner[1].height + self->corner[2].height);
}

static inline gboolean G_GNUC_PURE
node_supports_transform (const GskRenderNode *node)
{
  /* Some nodes can't handle non-trivial transforms without being
   * rendered to a texture (e.g. rotated clips, etc.). Some however work
   * just fine, mostly because they already draw their child to a
   * texture and just render the texture manipulated in some way, think
   * opacity or color matrix.
   */

  switch ((int)gsk_render_node_get_node_type (node))
    {
      case GSK_COLOR_NODE:
      case GSK_OPACITY_NODE:
      case GSK_COLOR_MATRIX_NODE:
      case GSK_TEXTURE_NODE:
      case GSK_CROSS_FADE_NODE:
      case GSK_LINEAR_GRADIENT_NODE:
      case GSK_DEBUG_NODE:
      case GSK_TEXT_NODE:
        return TRUE;

      case GSK_TRANSFORM_NODE:
        return node_supports_transform (gsk_transform_node_get_child (node));

      default:
        return FALSE;
    }
}

static inline gboolean G_GNUC_PURE
color_matrix_modifies_alpha (const GskRenderNode *node)
{
  const graphene_matrix_t *matrix = gsk_color_matrix_node_get_color_matrix (node);
  const graphene_vec4_t *offset = gsk_color_matrix_node_get_color_offset (node);
  graphene_vec4_t row3;

  if (graphene_vec4_get_w (offset) != 0.0f)
    return TRUE;

  graphene_matrix_get_row (matrix, 3, &row3);

  return !graphene_vec4_equal (graphene_vec4_w_axis (), &row3);
}

static inline gboolean G_GNUC_PURE
rect_contains_rect (const graphene_rect_t *r1,
                    const graphene_rect_t *r2)
{
  return r2->origin.x >= r1->origin.x &&
         (r2->origin.x + r2->size.width) <= (r1->origin.x + r1->size.width) &&
         r2->origin.y >= r1->origin.y &&
         (r2->origin.y + r2->size.height) <= (r1->origin.y + r1->size.height);
}

static inline gboolean
rounded_inner_rect_contains_rect (const GskRoundedRect  *rounded,
                                  const graphene_rect_t *rect)
{
  const graphene_rect_t *rounded_bounds = &rounded->bounds;
  graphene_rect_t inner;
  float offset_x;
  float offset_y;

  /* TODO: This is pretty conservative and we could go further,
   *       more fine-grained checks to avoid offscreen drawing.
   */

  offset_x = MAX (rounded->corner[GSK_CORNER_TOP_LEFT].width,
                  rounded->corner[GSK_CORNER_BOTTOM_LEFT].width);
  offset_y = MAX (rounded->corner[GSK_CORNER_TOP_LEFT].height,
                  rounded->corner[GSK_CORNER_TOP_RIGHT].height);

  inner.origin.x = rounded_bounds->origin.x + offset_x;
  inner.origin.y = rounded_bounds->origin.y + offset_y;
  inner.size.width = rounded_bounds->size.width - offset_x -
                     MAX (rounded->corner[GSK_CORNER_TOP_RIGHT].width,
                          rounded->corner[GSK_CORNER_BOTTOM_RIGHT].width);
  inner.size.height = rounded_bounds->size.height - offset_y -
                      MAX (rounded->corner[GSK_CORNER_BOTTOM_LEFT].height,
                           rounded->corner[GSK_CORNER_BOTTOM_RIGHT].height);

  return rect_contains_rect (&inner, rect);
}

static inline gboolean G_GNUC_PURE
rect_intersects (const graphene_rect_t *r1,
                 const graphene_rect_t *r2)
{
  /* Assume both rects are already normalized, as they usually are */
  if (r1->origin.x > (r2->origin.x + r2->size.width) ||
      (r1->origin.x + r1->size.width) < r2->origin.x)
    return FALSE;
  else if (r1->origin.y > (r2->origin.y + r2->size.height) ||
      (r1->origin.y + r1->size.height) < r2->origin.y)
    return FALSE;
  else
    return TRUE;
}

static inline gboolean
rounded_rect_has_corner (const GskRoundedRect *r,
                         guint                 i)
{
  return r->corner[i].width > 0 && r->corner[i].height > 0;
}

/* Current clip is NOT rounded but new one is definitely! */
static inline gboolean
intersect_rounded_rectilinear (const graphene_rect_t *non_rounded,
                               const GskRoundedRect  *rounded,
                               GskRoundedRect        *result)
{
  gboolean corners[4];

  /* Intersects with top left corner? */
  corners[0] = rounded_rect_has_corner (rounded, 0) &&
               rect_intersects (non_rounded,
                                &rounded_rect_corner (rounded, 0));
  /* top right? */
  corners[1] = rounded_rect_has_corner (rounded, 1) &&
               rect_intersects (non_rounded,
                                &rounded_rect_corner (rounded, 1));
  /* bottom right? */
  corners[2] = rounded_rect_has_corner (rounded, 2) &&
               rect_intersects (non_rounded,
                                &rounded_rect_corner (rounded, 2));
  /* bottom left */
  corners[3] = rounded_rect_has_corner (rounded, 3) &&
               rect_intersects (non_rounded,
                                &rounded_rect_corner (rounded, 3));

  if (corners[0] && !rect_contains_rect (non_rounded, &rounded_rect_corner (rounded, 0)))
    return FALSE;
  if (corners[1] && !rect_contains_rect (non_rounded, &rounded_rect_corner (rounded, 1)))
    return FALSE;
  if (corners[2] && !rect_contains_rect (non_rounded, &rounded_rect_corner (rounded, 2)))
    return FALSE;
  if (corners[3] && !rect_contains_rect (non_rounded, &rounded_rect_corner (rounded, 3)))
    return FALSE;

  /* We do intersect with at least one of the corners, but in such a way that the
   * intersection between the two clips can still be represented by a single rounded
   * rect in a trivial way. do that. */
  graphene_rect_intersection (non_rounded, &rounded->bounds, &result->bounds);

  for (guint i = 0; i < 4; i++)
    {
      if (corners[i])
        result->corner[i] = rounded->corner[i];
      else
        result->corner[i].width = result->corner[i].height = 0;
    }

  return TRUE;
}

static inline void
init_projection_matrix (graphene_matrix_t     *projection,
                        const graphene_rect_t *viewport)
{
  graphene_matrix_init_ortho (projection,
                              viewport->origin.x,
                              viewport->origin.x + viewport->size.width,
                              viewport->origin.y,
                              viewport->origin.y + viewport->size.height,
                              ORTHO_NEAR_PLANE,
                              ORTHO_FAR_PLANE);
  graphene_matrix_scale (projection, 1, -1, 1);
}

static inline float
gsk_ngl_render_job_set_alpha (GskNglRenderJob *job,
                              float            alpha)
{
  if (job->alpha != alpha)
    {
      float ret = job->alpha;
      job->alpha = alpha;
      job->driver->stamps[UNIFORM_SHARED_ALPHA]++;
      return ret;
    }

  return alpha;
}

static inline void
gsk_ngl_render_job_set_color (GskNglRenderJob *job,
                              const GdkRGBA   *color)
{
  job->color = *color;
}

static void
extract_matrix_metadata (GskNglRenderModelview *modelview)
{
  float dummy;
  graphene_matrix_t m;

  gsk_transform_to_matrix (modelview->transform, &modelview->matrix);

  switch (gsk_transform_get_category (modelview->transform))
    {
    case GSK_TRANSFORM_CATEGORY_IDENTITY:
    case GSK_TRANSFORM_CATEGORY_2D_TRANSLATE:
      modelview->scale_x = 1;
      modelview->scale_y = 1;
      break;

    case GSK_TRANSFORM_CATEGORY_2D_AFFINE:
      gsk_transform_to_affine (modelview->transform,
                               &modelview->scale_x, &modelview->scale_y,
                               &dummy, &dummy);
      break;

    case GSK_TRANSFORM_CATEGORY_UNKNOWN:
    case GSK_TRANSFORM_CATEGORY_ANY:
    case GSK_TRANSFORM_CATEGORY_3D:
    case GSK_TRANSFORM_CATEGORY_2D:
      {
        graphene_vec3_t col1;
        graphene_vec3_t col2;

        /* TODO: 90% sure this is incorrect. But we should never hit this code
         * path anyway. */
        graphene_vec3_init (&col1,
                            graphene_matrix_get_value (&m, 0, 0),
                            graphene_matrix_get_value (&m, 1, 0),
                            graphene_matrix_get_value (&m, 2, 0));

        graphene_vec3_init (&col2,
                            graphene_matrix_get_value (&m, 0, 1),
                            graphene_matrix_get_value (&m, 1, 1),
                            graphene_matrix_get_value (&m, 2, 1));

        modelview->scale_x = graphene_vec3_length (&col1);
        modelview->scale_y = graphene_vec3_length (&col2);
      }
      break;

    default:
      break;
    }
}

static void
gsk_ngl_render_job_set_modelview (GskNglRenderJob *job,
                                  GskTransform    *transform)
{
  GskNglRenderModelview *modelview;

  g_assert (job != NULL);
  g_assert (job->modelview != NULL);

  job->driver->stamps[UNIFORM_SHARED_MODELVIEW]++;

  g_array_set_size (job->modelview, job->modelview->len + 1);

  modelview = &g_array_index (job->modelview,
                              GskNglRenderModelview,
                              job->modelview->len - 1);

  modelview->transform = transform;

  modelview->offset_x_before = job->offset_x;
  modelview->offset_y_before = job->offset_y;

  extract_matrix_metadata (modelview);

  job->offset_x = 0;
  job->offset_y = 0;
  job->scale_x = modelview->scale_x;
  job->scale_y = modelview->scale_y;

  job->current_modelview = modelview;
}

static void
gsk_ngl_render_job_push_modelview (GskNglRenderJob *job,
                                   GskTransform    *transform)
{
  GskNglRenderModelview *modelview;

  g_assert (job != NULL);
  g_assert (job->modelview != NULL);
  g_assert (transform != NULL);

  job->driver->stamps[UNIFORM_SHARED_MODELVIEW]++;

  g_array_set_size (job->modelview, job->modelview->len + 1);

  modelview = &g_array_index (job->modelview,
                              GskNglRenderModelview,
                              job->modelview->len - 1);

  if G_LIKELY (job->modelview->len > 1)
    {
      GskNglRenderModelview *last;
      GskTransform *t = NULL;

      last = &g_array_index (job->modelview,
                             GskNglRenderModelview,
                             job->modelview->len - 2);

      /* Multiply given matrix with our previews modelview */
      t = gsk_transform_translate (gsk_transform_ref (last->transform),
                                   &(graphene_point_t) {
                                     job->offset_x,
                                     job->offset_y
                                   });
      t = gsk_transform_transform (t, transform);
      modelview->transform = t;
    }
  else
    {
      modelview->transform = gsk_transform_ref (transform);
    }

  modelview->offset_x_before = job->offset_x;
  modelview->offset_y_before = job->offset_y;

  extract_matrix_metadata (modelview);

  job->offset_x = 0;
  job->offset_y = 0;
  job->scale_x = modelview->scale_x;
  job->scale_y = modelview->scale_y;

  job->current_modelview = modelview;
}

static void
gsk_ngl_render_job_pop_modelview (GskNglRenderJob *job)
{
  const GskNglRenderModelview *head;

  g_assert (job != NULL);
  g_assert (job->modelview);
  g_assert (job->modelview->len > 0);

  job->driver->stamps[UNIFORM_SHARED_MODELVIEW]++;

  head = job->current_modelview;

  job->offset_x = head->offset_x_before;
  job->offset_y = head->offset_y_before;

  gsk_transform_unref (head->transform);

  job->modelview->len--;

  if (job->modelview->len >= 1)
    {
      head = &g_array_index (job->modelview, GskNglRenderModelview, job->modelview->len - 1);

      job->scale_x = head->scale_x;
      job->scale_y = head->scale_y;

      job->current_modelview = head;
    }
  else
    {
      job->current_modelview = NULL;
    }
}

static void
gsk_ngl_render_job_push_clip (GskNglRenderJob      *job,
                              const GskRoundedRect *rect)
{
  GskNglRenderClip *clip;

  g_assert (job != NULL);
  g_assert (job->clip != NULL);
  g_assert (rect != NULL);

  job->driver->stamps[UNIFORM_SHARED_CLIP_RECT]++;

  g_array_set_size (job->clip, job->clip->len + 1);

  clip = &g_array_index (job->clip, GskNglRenderClip, job->clip->len - 1);
  memcpy (&clip->rect, rect, sizeof *rect);
  clip->is_rectilinear = gsk_rounded_rect_is_rectilinear (rect);
  clip->is_fully_contained = FALSE;

  job->current_clip = clip;
}

static void
gsk_ngl_render_job_push_contained_clip (GskNglRenderJob *job)
{
  GskNglRenderClip *clip;
  GskNglRenderClip *old_clip;

  g_assert (job != NULL);
  g_assert (job->clip != NULL);
  g_assert (job->clip->len > 0);

  job->driver->stamps[UNIFORM_SHARED_CLIP_RECT]++;

  old_clip = &g_array_index (job->clip, GskNglRenderClip, job->clip->len - 1);

  g_array_set_size (job->clip, job->clip->len + 1);

  clip = &g_array_index (job->clip, GskNglRenderClip, job->clip->len - 1);
  memcpy (&clip->rect.bounds, &old_clip->rect.bounds, sizeof (graphene_rect_t));
  memset (clip->rect.corner, 0, sizeof clip->rect.corner);
  clip->is_rectilinear = TRUE;
  clip->is_fully_contained = TRUE;

  job->current_clip = clip;
}

static void
gsk_ngl_render_job_pop_clip (GskNglRenderJob *job)
{
  g_assert (job != NULL);
  g_assert (job->clip != NULL);
  g_assert (job->clip->len > 0);

  job->driver->stamps[UNIFORM_SHARED_CLIP_RECT]++;
  job->current_clip--;
  job->clip->len--;
}

static inline void
gsk_ngl_render_job_offset (GskNglRenderJob *job,
                           float            offset_x,
                           float            offset_y)
{
  if (offset_x || offset_y)
    {
      job->offset_x += offset_x;
      job->offset_y += offset_y;
    }
}

static inline void
gsk_ngl_render_job_set_projection (GskNglRenderJob         *job,
                                   const graphene_matrix_t *projection)
{
  memcpy (&job->projection, projection, sizeof job->projection);
  job->driver->stamps[UNIFORM_SHARED_PROJECTION]++;
}

static inline void
gsk_ngl_render_job_set_projection_from_rect (GskNglRenderJob       *job,
                                             const graphene_rect_t *rect,
                                             graphene_matrix_t     *prev_projection)
{
  if (prev_projection)
    memcpy (prev_projection, &job->projection, sizeof *prev_projection);
  init_projection_matrix (&job->projection, rect);
  job->driver->stamps[UNIFORM_SHARED_PROJECTION]++;
}

static inline void
gsk_ngl_render_job_set_projection_for_size (GskNglRenderJob   *job,
                                            float              width,
                                            float              height,
                                            graphene_matrix_t *prev_projection)
{
  if (prev_projection)
    memcpy (prev_projection, &job->projection, sizeof *prev_projection);
  graphene_matrix_init_ortho (&job->projection, 0, width, 0, height, ORTHO_NEAR_PLANE, ORTHO_FAR_PLANE);
  graphene_matrix_scale (&job->projection, 1, -1, 1);
  job->driver->stamps[UNIFORM_SHARED_PROJECTION]++;
}

static inline void
gsk_ngl_render_job_set_viewport (GskNglRenderJob       *job,
                                 const graphene_rect_t *viewport,
                                 graphene_rect_t       *prev_viewport)
{
  if (prev_viewport)
    memcpy (prev_viewport, &job->viewport, sizeof *prev_viewport);
  memcpy (&job->viewport, viewport, sizeof job->viewport);
  job->driver->stamps[UNIFORM_SHARED_VIEWPORT]++;
}

static inline void
gsk_ngl_render_job_set_viewport_for_size (GskNglRenderJob *job,
                                          float            width,
                                          float            height,
                                          graphene_rect_t *prev_viewport)
{
  if (prev_viewport)
    memcpy (prev_viewport, &job->viewport, sizeof *prev_viewport);
  job->viewport.origin.x = 0;
  job->viewport.origin.y = 0;
  job->viewport.size.width = width;
  job->viewport.size.height = height;
  job->driver->stamps[UNIFORM_SHARED_VIEWPORT]++;
}

static inline void
gsk_ngl_render_job_transform_bounds (GskNglRenderJob       *job,
                                     const graphene_rect_t *rect,
                                     graphene_rect_t       *out_rect)
{
  GskTransform *transform;
  GskTransformCategory category;

  g_assert (job != NULL);
  g_assert (job->modelview->len > 0);
  g_assert (rect != NULL);
  g_assert (out_rect != NULL);

  transform = job->current_modelview->transform;
  category = gsk_transform_get_category (transform);

  /* Our most common transform is 2d-affine, so inline it.
   * Both identity and 2d-translate are virtually unseen here.
   */
  if G_LIKELY (category == GSK_TRANSFORM_CATEGORY_2D_AFFINE)
    {
      float dx, dy, scale_x, scale_y;

      gsk_transform_to_affine (transform, &scale_x, &scale_y, &dx, &dy);

      /* Init directly into out rect */
      out_rect->origin.x = ((rect->origin.x + job->offset_x) * scale_x) + dx;
      out_rect->origin.y = ((rect->origin.y + job->offset_y) * scale_y) + dy;
      out_rect->size.width = rect->size.width * scale_x;
      out_rect->size.height = rect->size.height * scale_y;

      /* Normaize in place */
      if (out_rect->size.width < 0.f)
        {
          float size = fabsf (out_rect->size.width);

          out_rect->origin.x -= size;
          out_rect->size.width = size;
        }

      if (out_rect->size.height < 0.f)
        {
          float size = fabsf (out_rect->size.height);

          out_rect->origin.y -= size;
          out_rect->size.height = size;
        }
    }
  else
    {
      graphene_rect_t r;

      r.origin.x = rect->origin.x + job->offset_x;
      r.origin.y = rect->origin.y + job->offset_y;
      r.size.width = rect->size.width;
      r.size.height = rect->size.height;

      gsk_transform_transform_bounds (transform, &r, out_rect);
    }
}

static inline void
gsk_ngl_render_job_transform_rounded_rect (GskNglRenderJob      *job,
                                           const GskRoundedRect *rect,
                                           GskRoundedRect       *out_rect)
{
  out_rect->bounds.origin.x = job->offset_x + rect->bounds.origin.x;
  out_rect->bounds.origin.y = job->offset_y + rect->bounds.origin.y;
  out_rect->bounds.size.width = rect->bounds.size.width;
  out_rect->bounds.size.height = rect->bounds.size.height;
  memcpy (out_rect->corner, rect->corner, sizeof rect->corner);
}

static inline void
rounded_rect_get_inner (const GskRoundedRect *rect,
                        graphene_rect_t      *inner)
{
  float left = MAX (rect->corner[GSK_CORNER_TOP_LEFT].width, rect->corner[GSK_CORNER_BOTTOM_LEFT].width);
  float right = MAX (rect->corner[GSK_CORNER_TOP_RIGHT].width, rect->corner[GSK_CORNER_BOTTOM_RIGHT].width);
  float top = MAX (rect->corner[GSK_CORNER_TOP_LEFT].height, rect->corner[GSK_CORNER_TOP_RIGHT].height);
  float bottom = MAX (rect->corner[GSK_CORNER_BOTTOM_LEFT].height, rect->corner[GSK_CORNER_BOTTOM_RIGHT].height);

  inner->origin.x = rect->bounds.origin.x + left;
  inner->size.width = rect->bounds.size.width - (left + right);

  inner->origin.y = rect->bounds.origin.y + top;
  inner->size.height = rect->bounds.size.height - (top + bottom);
}

static inline gboolean
interval_contains (float p1, float w1,
                   float p2, float w2)
{
  if (p2 < p1)
    return FALSE;

  if (p2 + w2 > p1 + w1)
    return FALSE;

  return TRUE;
}

static inline gboolean
gsk_ngl_render_job_update_clip (GskNglRenderJob     *job,
                                const GskRenderNode *node,
                                gboolean            *pushed_clip)
{
  graphene_rect_t transformed_bounds;
  gboolean no_clip = FALSE;
  gboolean rect_clip = FALSE;

  *pushed_clip = FALSE;

  if (job->current_clip->is_fully_contained)
    {
      /* Already fully contained - no further checks needed */
      return TRUE;
    }

  gsk_ngl_render_job_transform_bounds (job, &node->bounds, &transformed_bounds);

  if (!rect_intersects (&job->current_clip->rect.bounds, &transformed_bounds))
    {
      /* Completely clipped away */
      return FALSE;
    }

  if (job->current_clip->is_rectilinear)
    {
      if (rect_contains_rect (&job->current_clip->rect.bounds, &transformed_bounds))
        no_clip = TRUE;
      else
        rect_clip = TRUE;
    }
  else if (gsk_rounded_rect_contains_rect (&job->current_clip->rect, &transformed_bounds))
    {
      no_clip = TRUE;
    }
  else
    {
      graphene_rect_t inner;

      rounded_rect_get_inner (&job->current_clip->rect, &inner);

      if (interval_contains (inner.origin.x, inner.size.width,
                             transformed_bounds.origin.x, transformed_bounds.size.width) ||
          interval_contains (inner.origin.y, inner.size.height,
                             transformed_bounds.origin.y, transformed_bounds.size.height))
        rect_clip = TRUE;
    }

  if (no_clip)
    {
      /* This node is completely contained inside the clip.
       * Record this fact on the clip stack, so we don't do more work
       * for child nodes.
       */

      gsk_ngl_render_job_push_contained_clip (job);

      *pushed_clip = TRUE;
    }
  else if (rect_clip && !job->current_clip->is_rectilinear)
    {
      graphene_rect_t rect;

      /* The clip gets simpler for this node */

      graphene_rect_intersection (&job->current_clip->rect.bounds, &transformed_bounds, &rect);
      gsk_ngl_render_job_push_clip (job, &GSK_ROUNDED_RECT_INIT_FROM_RECT (rect));

      *pushed_clip = TRUE;
    }

  return TRUE;
}

/* fill_vertex_data */
static void
gsk_ngl_render_job_draw_coords (GskNglRenderJob *job,
                                float            min_x,
                                float            min_y,
                                float            max_x,
                                float            max_y,
                                float            min_u,
                                float            min_v,
                                float            max_u,
                                float            max_v)
{
  GskNglDrawVertex *vertices = gsk_ngl_command_queue_add_vertices (job->command_queue);

  vertices[0].position[0] = min_x;
  vertices[0].position[1] = min_y;
  vertices[0].uv[0] = min_u;
  vertices[0].uv[1] = min_v;
  vertices[0].color[0] = job->color.red;
  vertices[0].color[1] = job->color.green;
  vertices[0].color[2] = job->color.blue;
  vertices[0].color[3] = job->color.alpha;

  vertices[1].position[0] = min_x;
  vertices[1].position[1] = max_y;
  vertices[1].uv[0] = min_u;
  vertices[1].uv[1] = max_v;
  vertices[1].color[0] = job->color.red;
  vertices[1].color[1] = job->color.green;
  vertices[1].color[2] = job->color.blue;
  vertices[1].color[3] = job->color.alpha;

  vertices[2].position[0] = max_x;
  vertices[2].position[1] = min_y;
  vertices[2].uv[0] = max_u;
  vertices[2].uv[1] = min_v;
  vertices[2].color[0] = job->color.red;
  vertices[2].color[1] = job->color.green;
  vertices[2].color[2] = job->color.blue;
  vertices[2].color[3] = job->color.alpha;

  vertices[3].position[0] = max_x;
  vertices[3].position[1] = max_y;
  vertices[3].uv[0] = max_u;
  vertices[3].uv[1] = max_v;
  vertices[3].color[0] = job->color.red;
  vertices[3].color[1] = job->color.green;
  vertices[3].color[2] = job->color.blue;
  vertices[3].color[3] = job->color.alpha;

  vertices[4].position[0] = min_x;
  vertices[4].position[1] = max_y;
  vertices[4].uv[0] = min_u;
  vertices[4].uv[1] = max_v;
  vertices[4].color[0] = job->color.red;
  vertices[4].color[1] = job->color.green;
  vertices[4].color[2] = job->color.blue;
  vertices[4].color[3] = job->color.alpha;

  vertices[5].position[0] = max_x;
  vertices[5].position[1] = min_y;
  vertices[5].uv[0] = max_u;
  vertices[5].uv[1] = min_v;
  vertices[5].color[0] = job->color.red;
  vertices[5].color[1] = job->color.green;
  vertices[5].color[2] = job->color.blue;
  vertices[5].color[3] = job->color.alpha;
}

/* load_vertex_data_with_region */
static inline void
gsk_ngl_render_job_load_vertices_from_offscreen (GskNglRenderJob             *job,
                                                 const graphene_rect_t       *bounds,
                                                 const GskNglRenderOffscreen *offscreen)
{
  float min_x = job->offset_x + bounds->origin.x;
  float min_y = job->offset_y + bounds->origin.y;
  float max_x = min_x + bounds->size.width;
  float max_y = min_y + bounds->size.height;
  float y1 = offscreen->was_offscreen ? offscreen->area.y2 : offscreen->area.y;
  float y2 = offscreen->was_offscreen ? offscreen->area.y : offscreen->area.y2;

  gsk_ngl_render_job_draw_coords (job,
                                  min_x, min_y, max_x, max_y,
                                  offscreen->area.x, y1, offscreen->area.x2, y2);
}

/* load_float_vertex_data */
static inline void
gsk_ngl_render_job_draw (GskNglRenderJob *job,
                         float            x,
                         float            y,
                         float            width,
                         float            height)
{
  float min_x = job->offset_x + x;
  float min_y = job->offset_y + y;
  float max_x = min_x + width;
  float max_y = min_y + height;

  gsk_ngl_render_job_draw_coords (job, min_x, min_y, max_x, max_y, 0, 0, 1, 1);
}

/* load_vertex_data */
static inline void
gsk_ngl_render_job_draw_rect (GskNglRenderJob       *job,
                              const graphene_rect_t *bounds)
{
  gsk_ngl_render_job_draw (job,
                           bounds->origin.x,
                           bounds->origin.y,
                           bounds->size.width,
                           bounds->size.height);
}

/* load_offscreen_vertex_data */
static inline void
gsk_ngl_render_job_draw_offscreen_rect (GskNglRenderJob       *job,
                                        const graphene_rect_t *bounds)
{
  float min_x = job->offset_x + bounds->origin.x;
  float min_y = job->offset_y + bounds->origin.y;
  float max_x = min_x + bounds->size.width;
  float max_y = min_y + bounds->size.height;

  gsk_ngl_render_job_draw_coords (job, min_x, min_y, max_x, max_y, 0, 1, 1, 0);
}

static inline void
gsk_ngl_render_job_begin_draw (GskNglRenderJob *job,
                               GskNglProgram   *program)
{
  job->current_program = program;

  gsk_ngl_command_queue_begin_draw (job->command_queue,
                                    program->program_info,
                                    job->viewport.size.width,
                                    job->viewport.size.height);

  if (program->uniform_locations[UNIFORM_SHARED_VIEWPORT] > -1)
    gsk_ngl_uniform_state_set4fv (program->uniforms,
                                  program->program_info,
                                  program->uniform_locations[UNIFORM_SHARED_VIEWPORT],
                                  job->driver->stamps[UNIFORM_SHARED_VIEWPORT],
                                  1,
                                  (const float *)&job->viewport);

  if (program->uniform_locations[UNIFORM_SHARED_MODELVIEW] > -1)
    gsk_ngl_uniform_state_set_matrix (program->uniforms,
                                      program->program_info,
                                      program->uniform_locations[UNIFORM_SHARED_MODELVIEW],
                                      job->driver->stamps[UNIFORM_SHARED_MODELVIEW],
                                      &job->current_modelview->matrix);

  if (program->uniform_locations[UNIFORM_SHARED_PROJECTION] > -1)
    gsk_ngl_uniform_state_set_matrix (program->uniforms,
                                      program->program_info,
                                      program->uniform_locations[UNIFORM_SHARED_PROJECTION],
                                      job->driver->stamps[UNIFORM_SHARED_PROJECTION],
                                      &job->projection);

  if (program->uniform_locations[UNIFORM_SHARED_CLIP_RECT] > -1)
    gsk_ngl_uniform_state_set_rounded_rect (program->uniforms,
                                            program->program_info,
                                            program->uniform_locations[UNIFORM_SHARED_CLIP_RECT],
                                            job->driver->stamps[UNIFORM_SHARED_CLIP_RECT],
                                            &job->current_clip->rect);

  if (program->uniform_locations[UNIFORM_SHARED_ALPHA] > -1)
    gsk_ngl_uniform_state_set1f (program->uniforms,
                                 program->program_info,
                                 program->uniform_locations[UNIFORM_SHARED_ALPHA],
                                 job->driver->stamps[UNIFORM_SHARED_ALPHA],
                                 job->alpha);
}

#define CHOOSE_PROGRAM(job,name) \
  (job->current_clip->is_fully_contained \
      ? job->driver->name ## _no_clip \
      : (job->current_clip->is_rectilinear \
        ? job->driver->name ## _rect_clip \
        : job->driver->name))

static inline void
gsk_ngl_render_job_split_draw (GskNglRenderJob *job)
{
  gsk_ngl_command_queue_split_draw (job->command_queue);
}

static inline void
gsk_ngl_render_job_end_draw (GskNglRenderJob *job)
{
  gsk_ngl_command_queue_end_draw (job->command_queue);

  job->current_program = NULL;
}

static inline void
gsk_ngl_render_job_visit_as_fallback (GskNglRenderJob     *job,
                                      const GskRenderNode *node)
{
  float scale_x = job->scale_x;
  float scale_y = job->scale_y;
  int surface_width = ceilf (node->bounds.size.width * scale_x);
  int surface_height = ceilf (node->bounds.size.height * scale_y);
  GdkTexture *texture;
  cairo_surface_t *surface;
  cairo_surface_t *rendered_surface;
  cairo_t *cr;
  int cached_id;
  int texture_id;
  GskTextureKey key;

  if (surface_width <= 0 || surface_height <= 0)
    return;

  key.pointer = node;
  key.pointer_is_child = FALSE;
  key.scale_x = scale_x;
  key.scale_y = scale_y;
  key.filter = GL_NEAREST;

  cached_id = gsk_ngl_driver_lookup_texture (job->driver, &key);

  if (cached_id != 0)
    {
      gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blit));
      gsk_ngl_program_set_uniform_texture (job->current_program,
                                           UNIFORM_SHARED_SOURCE, 0,
                                           GL_TEXTURE_2D, GL_TEXTURE0, cached_id);
      gsk_ngl_render_job_draw_offscreen_rect (job, &node->bounds);
      gsk_ngl_render_job_end_draw (job);
      return;
    }

  /* We first draw the recording surface on an image surface,
   * just because the scaleY(-1) later otherwise screws up the
   * rendering... */
  {
    rendered_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                   surface_width,
                                                   surface_height);

    cairo_surface_set_device_scale (rendered_surface, scale_x, scale_y);
    cr = cairo_create (rendered_surface);

    cairo_save (cr);
    cairo_translate (cr, - floorf (node->bounds.origin.x), - floorf (node->bounds.origin.y));
    /* Render nodes don't modify state, so casting away the const is fine here */
    gsk_render_node_draw ((GskRenderNode *)node, cr);
    cairo_restore (cr);
    cairo_destroy (cr);
  }

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                        surface_width,
                                        surface_height);
  cairo_surface_set_device_scale (surface, scale_x, scale_y);
  cr = cairo_create (surface);

  /* We draw upside down here, so it matches what GL does. */
  cairo_save (cr);
  cairo_scale (cr, 1, -1);
  cairo_translate (cr, 0, - surface_height / scale_y);
  cairo_set_source_surface (cr, rendered_surface, 0, 0);
  cairo_rectangle (cr, 0, 0, surface_width / scale_x, surface_height / scale_y);
  cairo_fill (cr);
  cairo_restore (cr);

#ifdef G_ENABLE_DEBUG
  if (job->debug_fallback)
    {
      cairo_move_to (cr, 0, 0);
      cairo_rectangle (cr, 0, 0, node->bounds.size.width, node->bounds.size.height);
      if (gsk_render_node_get_node_type (node) == GSK_CAIRO_NODE)
        cairo_set_source_rgba (cr, 0.3, 0, 1, 0.25);
      else
        cairo_set_source_rgba (cr, 1, 0, 0, 0.25);
      cairo_fill_preserve (cr);
      if (gsk_render_node_get_node_type (node) == GSK_CAIRO_NODE)
        cairo_set_source_rgba (cr, 0.3, 0, 1, 1);
      else
        cairo_set_source_rgba (cr, 1, 0, 0, 1);
      cairo_stroke (cr);
    }
#endif
  cairo_destroy (cr);

  /* Create texture to upload */
  texture = gdk_texture_new_for_surface (surface);
  texture_id = gsk_ngl_driver_load_texture (job->driver, texture,
                                            GL_NEAREST, GL_NEAREST);

  if (gdk_gl_context_has_debug (job->command_queue->context))
    gdk_gl_context_label_object_printf (job->command_queue->context, GL_TEXTURE, texture_id,
                                        "Fallback %s %d",
                                        g_type_name_from_instance ((GTypeInstance *) node),
                                        texture_id);

  g_object_unref (texture);
  cairo_surface_destroy (surface);
  cairo_surface_destroy (rendered_surface);

  gsk_ngl_driver_cache_texture (job->driver, &key, texture_id);

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blit));
  gsk_ngl_program_set_uniform_texture (job->current_program,
                                       UNIFORM_SHARED_SOURCE, 0,
                                       GL_TEXTURE_2D,
                                       GL_TEXTURE0,
                                       texture_id);
  gsk_ngl_render_job_draw_offscreen_rect (job, &node->bounds);
  gsk_ngl_render_job_end_draw (job);
}

static guint
blur_offscreen (GskNglRenderJob       *job,
                GskNglRenderOffscreen *offscreen,
                int                   texture_to_blur_width,
                int                   texture_to_blur_height,
                float                 blur_radius_x,
                float                 blur_radius_y)
{
  const GskRoundedRect new_clip = GSK_ROUNDED_RECT_INIT (0, 0, texture_to_blur_width, texture_to_blur_height);
  GskNglRenderTarget *pass1;
  GskNglRenderTarget *pass2;
  graphene_matrix_t prev_projection;
  graphene_rect_t prev_viewport;
  guint prev_fbo;

  g_assert (blur_radius_x > 0);
  g_assert (blur_radius_y > 0);
  g_assert (offscreen->texture_id > 0);
  g_assert (offscreen->area.x2 > offscreen->area.x);
  g_assert (offscreen->area.y2 > offscreen->area.y);

  if (!gsk_ngl_driver_create_render_target (job->driver,
                                             MAX (texture_to_blur_width, 1),
                                             MAX (texture_to_blur_height, 1),
                                             GL_NEAREST, GL_NEAREST,
                                             &pass1))
    return 0;

  if (texture_to_blur_width <= 0 || texture_to_blur_height <= 0)
    return gsk_ngl_driver_release_render_target (job->driver, pass1, FALSE);

  if (!gsk_ngl_driver_create_render_target (job->driver,
                                             texture_to_blur_width,
                                             texture_to_blur_height,
                                             GL_NEAREST, GL_NEAREST,
                                             &pass2))
    return gsk_ngl_driver_release_render_target (job->driver, pass1, FALSE);

  gsk_ngl_render_job_set_viewport (job, &new_clip.bounds, &prev_viewport);
  gsk_ngl_render_job_set_projection_from_rect (job, &new_clip.bounds, &prev_projection);
  gsk_ngl_render_job_set_modelview (job, NULL);
  gsk_ngl_render_job_push_clip (job, &new_clip);

  /* Bind new framebuffer and clear it */
  prev_fbo = gsk_ngl_command_queue_bind_framebuffer (job->command_queue, pass1->framebuffer_id);
  gsk_ngl_command_queue_clear (job->command_queue, 0, &job->viewport);

  /* Begin drawing the first horizontal pass, using offscreen as the
   * source texture for the program.
   */
  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blur));
  gsk_ngl_program_set_uniform_texture (job->current_program,
                                       UNIFORM_SHARED_SOURCE, 0,
                                       GL_TEXTURE_2D,
                                       GL_TEXTURE0,
                                       offscreen->texture_id);
  gsk_ngl_program_set_uniform1f (job->current_program,
                                 UNIFORM_BLUR_RADIUS, 0,
                                 blur_radius_x);
  gsk_ngl_program_set_uniform2f (job->current_program,
                                 UNIFORM_BLUR_SIZE, 0,
                                 texture_to_blur_width,
                                 texture_to_blur_height);
  gsk_ngl_program_set_uniform2f (job->current_program,
                                 UNIFORM_BLUR_DIR, 0,
                                 1, 0);
  gsk_ngl_render_job_draw_coords (job, 0, 0, texture_to_blur_width, texture_to_blur_height, 0, 1, 1, 0);
  gsk_ngl_render_job_end_draw (job);

  /* Bind second pass framebuffer and clear it */
  gsk_ngl_command_queue_bind_framebuffer (job->command_queue, pass2->framebuffer_id);
  gsk_ngl_command_queue_clear (job->command_queue, 0, &job->viewport);

  /* Draw using blur program with first pass as source texture */
  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blur));
  gsk_ngl_program_set_uniform_texture (job->current_program,
                                       UNIFORM_SHARED_SOURCE, 0,
                                       GL_TEXTURE_2D,
                                       GL_TEXTURE0,
                                       pass1->texture_id);
  gsk_ngl_program_set_uniform1f (job->current_program,
                                 UNIFORM_BLUR_RADIUS, 0,
                                 blur_radius_y);
  gsk_ngl_program_set_uniform2f (job->current_program,
                                 UNIFORM_BLUR_SIZE, 0,
                                 texture_to_blur_width,
                                 texture_to_blur_height);
  gsk_ngl_program_set_uniform2f (job->current_program,
                                 UNIFORM_BLUR_DIR, 0,
                                 0, 1);
  gsk_ngl_render_job_draw_coords (job, 0, 0, texture_to_blur_width, texture_to_blur_height, 0, 1, 1, 0);
  gsk_ngl_render_job_end_draw (job);

  gsk_ngl_render_job_pop_modelview (job);
  gsk_ngl_render_job_pop_clip (job);
  gsk_ngl_render_job_set_viewport (job, &prev_viewport, NULL);
  gsk_ngl_render_job_set_projection (job, &prev_projection);
  gsk_ngl_command_queue_bind_framebuffer (job->command_queue, prev_fbo);

  gsk_ngl_driver_release_render_target (job->driver, pass1, TRUE);

  return gsk_ngl_driver_release_render_target (job->driver, pass2, FALSE);
}

static void
blur_node (GskNglRenderJob       *job,
           GskNglRenderOffscreen *offscreen,
           const GskRenderNode  *node,
           float                 blur_radius,
           float                *min_x,
           float                *max_x,
           float                *min_y,
           float                *max_y)
{
  const float blur_extra = blur_radius * 2.0; /* 2.0 = shader radius_multiplier */
  const float half_blur_extra = (blur_extra / 2.0);
  float scale_x = job->scale_x;
  float scale_y = job->scale_y;
  float texture_width;
  float texture_height;

  g_assert (blur_radius > 0);

  /* Increase texture size for the given blur radius and scale it */
  texture_width  = ceilf ((node->bounds.size.width  + blur_extra));
  texture_height = ceilf ((node->bounds.size.height + blur_extra));

  /* Only blur this if the out region has no texture id yet */
  if (offscreen->texture_id == 0)
    {
      const graphene_rect_t bounds = GRAPHENE_RECT_INIT (node->bounds.origin.x - half_blur_extra,
                                                         node->bounds.origin.y - half_blur_extra,
                                                         texture_width, texture_height);

      offscreen->bounds = &bounds;
      offscreen->reset_clip = TRUE;
      offscreen->force_offscreen = TRUE;

      if (!gsk_ngl_render_job_visit_node_with_offscreen (job, node, offscreen))
        g_assert_not_reached ();

      /* Ensure that we actually got a real texture_id */
      g_assert (offscreen->texture_id != 0);

      offscreen->texture_id = blur_offscreen (job,
                                              offscreen,
                                              texture_width * scale_x,
                                              texture_height * scale_y,
                                              blur_radius * scale_x,
                                              blur_radius * scale_y);
      init_full_texture_region (offscreen);
    }

  *min_x = job->offset_x + node->bounds.origin.x - half_blur_extra;
  *max_x = job->offset_x + node->bounds.origin.x + node->bounds.size.width + half_blur_extra;
  *min_y = job->offset_y + node->bounds.origin.y - half_blur_extra;
  *max_y = job->offset_y + node->bounds.origin.y + node->bounds.size.height + half_blur_extra;
}

static inline void
gsk_ngl_render_job_visit_color_node (GskNglRenderJob     *job,
                                     const GskRenderNode *node)
{
  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, color));
  gsk_ngl_render_job_set_color (job, gsk_color_node_get_color (node));
  gsk_ngl_render_job_draw_rect (job, &node->bounds);
  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_linear_gradient_node (GskNglRenderJob     *job,
                                               const GskRenderNode *node)
{
  const GskColorStop *stops = gsk_linear_gradient_node_get_color_stops (node, NULL);
  const graphene_point_t *start = gsk_linear_gradient_node_get_start (node);
  const graphene_point_t *end = gsk_linear_gradient_node_get_end (node);
  int n_color_stops = gsk_linear_gradient_node_get_n_color_stops (node);
  gboolean repeat = gsk_render_node_get_node_type (node) == GSK_REPEATING_LINEAR_GRADIENT_NODE;
  float x1 = job->offset_x + start->x;
  float x2 = job->offset_x + end->x;
  float y1 = job->offset_y + start->y;
  float y2 = job->offset_y + end->y;

  g_assert (n_color_stops < MAX_GRADIENT_STOPS);

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, linear_gradient));
  gsk_ngl_program_set_uniform1i (job->current_program,
                                 UNIFORM_LINEAR_GRADIENT_NUM_COLOR_STOPS, 0,
                                 n_color_stops);
  gsk_ngl_program_set_uniform1fv (job->current_program,
                                  UNIFORM_LINEAR_GRADIENT_COLOR_STOPS, 0,
                                  n_color_stops * 5,
                                  (const float *)stops);
  gsk_ngl_program_set_uniform4f (job->current_program,
                                 UNIFORM_LINEAR_GRADIENT_POINTS, 0,
                                 x1, y1, x2 - x1, y2 - y1);
  gsk_ngl_program_set_uniform1i (job->current_program,
                                 UNIFORM_LINEAR_GRADIENT_REPEAT, 0,
                                 repeat);
  gsk_ngl_render_job_draw_rect (job, &node->bounds);
  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_conic_gradient_node (GskNglRenderJob     *job,
                                              const GskRenderNode *node)
{
  static const float scale = 0.5f * M_1_PI;

  const GskColorStop *stops = gsk_conic_gradient_node_get_color_stops (node, NULL);
  const graphene_point_t *center = gsk_conic_gradient_node_get_center (node);
  int n_color_stops = gsk_conic_gradient_node_get_n_color_stops (node);
  float angle = gsk_conic_gradient_node_get_angle (node);
  float bias = angle * scale + 2.0f;

  g_assert (n_color_stops < MAX_GRADIENT_STOPS);

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, conic_gradient));
  gsk_ngl_program_set_uniform1i (job->current_program,
                                 UNIFORM_CONIC_GRADIENT_NUM_COLOR_STOPS, 0,
                                 n_color_stops);
  gsk_ngl_program_set_uniform1fv (job->current_program,
                                  UNIFORM_CONIC_GRADIENT_COLOR_STOPS, 0,
                                  n_color_stops * 5,
                                  (const float *)stops);
  gsk_ngl_program_set_uniform4f (job->current_program,
                                 UNIFORM_CONIC_GRADIENT_GEOMETRY, 0,
                                 job->offset_x + center->x,
                                 job->offset_y + center->y,
                                 scale,
                                 bias);
  gsk_ngl_render_job_draw_rect (job, &node->bounds);
  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_radial_gradient_node (GskNglRenderJob     *job,
                                               const GskRenderNode *node)
{
  int n_color_stops = gsk_radial_gradient_node_get_n_color_stops (node);
  const GskColorStop *stops = gsk_radial_gradient_node_get_color_stops (node, NULL);
  const graphene_point_t *center = gsk_radial_gradient_node_get_center (node);
  float start = gsk_radial_gradient_node_get_start (node);
  float end = gsk_radial_gradient_node_get_end (node);
  float hradius = gsk_radial_gradient_node_get_hradius (node);
  float vradius = gsk_radial_gradient_node_get_vradius (node);
  gboolean repeat = gsk_render_node_get_node_type (node) == GSK_REPEATING_RADIAL_GRADIENT_NODE;
  float scale = 1.0f / (end - start);
  float bias = -start * scale;

  g_assert (n_color_stops < MAX_GRADIENT_STOPS);

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, radial_gradient));
  gsk_ngl_program_set_uniform1i (job->current_program,
                                 UNIFORM_RADIAL_GRADIENT_NUM_COLOR_STOPS, 0,
                                 n_color_stops);
  gsk_ngl_program_set_uniform1fv (job->current_program,
                                  UNIFORM_RADIAL_GRADIENT_COLOR_STOPS, 0,
                                  n_color_stops * 5,
                                  (const float *)stops);
  gsk_ngl_program_set_uniform1i (job->current_program,
                                 UNIFORM_RADIAL_GRADIENT_REPEAT, 0,
                                 repeat);
  gsk_ngl_program_set_uniform2f (job->current_program,
                                 UNIFORM_RADIAL_GRADIENT_RANGE, 0,
                                 scale, bias);
  gsk_ngl_program_set_uniform4f (job->current_program,
                                 UNIFORM_RADIAL_GRADIENT_GEOMETRY, 0,
                                 job->offset_x + center->x,
                                 job->offset_y + center->y,
                                 1.0f / (hradius * job->scale_x),
                                 1.0f / (vradius * job->scale_y));
  gsk_ngl_render_job_draw_rect (job, &node->bounds);
  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_clipped_child (GskNglRenderJob       *job,
                                        const GskRenderNode   *child,
                                        const graphene_rect_t *clip)
{
  graphene_rect_t transformed_clip;
  GskRoundedRect intersection;

  gsk_ngl_render_job_transform_bounds (job, clip, &transformed_clip);

  if (job->current_clip->is_rectilinear)
    {
      memset (&intersection.corner, 0, sizeof intersection.corner);
      graphene_rect_intersection (&transformed_clip,
                                  &job->current_clip->rect.bounds,
                                  &intersection.bounds);

      gsk_ngl_render_job_push_clip (job, &intersection);
      gsk_ngl_render_job_visit_node (job, child);
      gsk_ngl_render_job_pop_clip (job);
    }
  else if (intersect_rounded_rectilinear (&transformed_clip,
                                          &job->current_clip->rect,
                                          &intersection))
    {
      gsk_ngl_render_job_push_clip (job, &intersection);
      gsk_ngl_render_job_visit_node (job, child);
      gsk_ngl_render_job_pop_clip (job);
    }
  else
    {
      GskRoundedRect scaled_clip;
      GskNglRenderOffscreen offscreen = {0};

      offscreen.bounds = &child->bounds;
      offscreen.force_offscreen = TRUE;

      scaled_clip = GSK_ROUNDED_RECT_INIT ((job->offset_x + clip->origin.x) * job->scale_x,
                                           (job->offset_y + clip->origin.y) * job->scale_y,
                                           clip->size.width * job->scale_x,
                                           clip->size.height * job->scale_y);

      gsk_ngl_render_job_push_clip (job, &scaled_clip);
      gsk_ngl_render_job_visit_node_with_offscreen (job, child, &offscreen);
      gsk_ngl_render_job_pop_clip (job);

      g_assert (offscreen.texture_id);

      gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blit));
      gsk_ngl_program_set_uniform_texture (job->current_program,
                                           UNIFORM_SHARED_SOURCE, 0,
                                           GL_TEXTURE_2D,
                                           GL_TEXTURE0,
                                           offscreen.texture_id);
      gsk_ngl_render_job_draw_offscreen_rect (job, &child->bounds);
      gsk_ngl_render_job_end_draw (job);
    }
}

static inline void
gsk_ngl_render_job_visit_clip_node (GskNglRenderJob     *job,
                                    const GskRenderNode *node)
{
  const graphene_rect_t *clip = gsk_clip_node_get_clip (node);
  const GskRenderNode *child = gsk_clip_node_get_child (node);

  gsk_ngl_render_job_visit_clipped_child (job, child, clip);
}

static inline void
gsk_ngl_render_job_visit_rounded_clip_node (GskNglRenderJob     *job,
                                            const GskRenderNode *node)
{
  const GskRenderNode *child = gsk_rounded_clip_node_get_child (node);
  const GskRoundedRect *clip = gsk_rounded_clip_node_get_clip (node);
  GskRoundedRect transformed_clip;
  float scale_x = job->scale_x;
  float scale_y = job->scale_y;
  gboolean need_offscreen;

  if (node_is_invisible (child))
    return;

  gsk_ngl_render_job_transform_bounds (job, &clip->bounds, &transformed_clip.bounds);

  for (guint i = 0; i < G_N_ELEMENTS (transformed_clip.corner); i++)
    {
      transformed_clip.corner[i].width = clip->corner[i].width * scale_x;
      transformed_clip.corner[i].height = clip->corner[i].height * scale_y;
    }

  if (job->current_clip->is_rectilinear)
    {
      GskRoundedRect intersected_clip;

      if (intersect_rounded_rectilinear (&job->current_clip->rect.bounds,
                                         &transformed_clip,
                                         &intersected_clip))
        {
          gsk_ngl_render_job_push_clip (job, &intersected_clip);
          gsk_ngl_render_job_visit_node (job, child);
          gsk_ngl_render_job_pop_clip (job);
          return;
        }
    }

  /* After this point we are really working with a new and a current clip
   * which both have rounded corners.
   */

  if (job->clip->len <= 1)
    need_offscreen = FALSE;
  else if (rounded_inner_rect_contains_rect (&job->current_clip->rect, &transformed_clip.bounds))
    need_offscreen = FALSE;
  else
    need_offscreen = TRUE;

  if (!need_offscreen)
    {
      /* If the new clip entirely contains the current clip, the intersection is simply
       * the current clip, so we can ignore the new one.
       */
      if (rounded_inner_rect_contains_rect (&transformed_clip, &job->current_clip->rect.bounds))
        {
          gsk_ngl_render_job_visit_node (job, child);
          return;
        }

      gsk_ngl_render_job_push_clip (job, &transformed_clip);
      gsk_ngl_render_job_visit_node (job, child);
      gsk_ngl_render_job_pop_clip (job);
    }
  else
    {
      GskNglRenderOffscreen offscreen = {0};

      offscreen.bounds = &node->bounds;
      offscreen.force_offscreen = TRUE;

      gsk_ngl_render_job_push_clip (job, &transformed_clip);
      if (!gsk_ngl_render_job_visit_node_with_offscreen (job, child, &offscreen))
        g_assert_not_reached ();
      gsk_ngl_render_job_pop_clip (job);

      g_assert (offscreen.texture_id);

      gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blit));
      gsk_ngl_program_set_uniform_texture (job->current_program,
                                           UNIFORM_SHARED_SOURCE, 0,
                                           GL_TEXTURE_2D,
                                           GL_TEXTURE0,
                                           offscreen.texture_id);
      gsk_ngl_render_job_load_vertices_from_offscreen (job, &node->bounds, &offscreen);
      gsk_ngl_render_job_end_draw (job);
    }
}

static inline void
sort_border_sides (const GdkRGBA *colors,
                   int           *indices)
{
  gboolean done[4] = {0, 0, 0, 0};
  guint cur = 0;

  for (guint i = 0; i < 3; i++)
    {
      if (done[i])
        continue;

      indices[cur] = i;
      done[i] = TRUE;
      cur++;

      for (guint k = i + 1; k < 4; k ++)
        {
          if (memcmp (&colors[k], &colors[i], sizeof (GdkRGBA)) == 0)
            {
              indices[cur] = k;
              done[k] = TRUE;
              cur++;
            }
        }

      if (cur >= 4)
        break;
    }
}

static inline void
gsk_ngl_render_job_visit_rect_border_node (GskNglRenderJob     *job,
                                           const GskRenderNode *node)
{
  const GdkRGBA *colors = gsk_border_node_get_colors (node);
  const float *widths = gsk_border_node_get_widths (node);
  const graphene_point_t *origin = &node->bounds.origin;
  const graphene_size_t *size = &node->bounds.size;

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, color));

  gsk_ngl_render_job_set_color (job, &colors[0]);

  gsk_ngl_render_job_draw_rect (job,
      &GRAPHENE_RECT_INIT (origin->x,
                           origin->y,
                           size->width - widths[1],
                           widths[0]));

  gsk_ngl_render_job_draw_rect (job,
      &GRAPHENE_RECT_INIT (origin->x + size->width - widths[1],
                           origin->y,
                           widths[1],
                           size->height - widths[2]));

  gsk_ngl_render_job_draw_rect (job,
      &GRAPHENE_RECT_INIT (origin->x + widths[3],
                           origin->y + size->height - widths[2],
                           size->width - widths[1],
                           widths[2]));

  gsk_ngl_render_job_draw_rect (job,
      &GRAPHENE_RECT_INIT (origin->x,
                           origin->y + widths[0],
                           widths[3],
                           size->height - widths[0]));

  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_uniform_border_node (GskNglRenderJob     *job,
                                              const GskRenderNode *node)
{
  const GskRoundedRect *rounded_outline = gsk_border_node_get_outline (node);
  const GdkRGBA *colors = gsk_border_node_get_colors (node);
  const float *widths = gsk_border_node_get_widths (node);
  GskRoundedRect outline;

  gsk_ngl_render_job_transform_rounded_rect (job, rounded_outline, &outline);

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, inset_shadow));
  gsk_ngl_render_job_set_color (job, &colors[0]);
  gsk_ngl_program_set_uniform_rounded_rect (job->current_program,
                                            UNIFORM_INSET_SHADOW_OUTLINE_RECT, 0,
                                            &outline);
  gsk_ngl_program_set_uniform1f (job->current_program,
                                 UNIFORM_INSET_SHADOW_SPREAD, 0,
                                 widths[0]);
  gsk_ngl_program_set_uniform2f (job->current_program,
                                 UNIFORM_INSET_SHADOW_OFFSET, 0,
                                 0, 0);
  gsk_ngl_render_job_draw_rect (job, &node->bounds);
  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_border_node (GskNglRenderJob     *job,
                                      const GskRenderNode *node)
{
  const GskRoundedRect *rounded_outline = gsk_border_node_get_outline (node);
  const GdkRGBA *c = gsk_border_node_get_colors (node);
  const float *widths = gsk_border_node_get_widths (node);
  struct {
    float w;
    float h;
  } sizes[4];

  /* Top left */
  if (widths[3] > 0)
    sizes[0].w = MAX (widths[3], rounded_outline->corner[0].width);
  else
    sizes[0].w = 0;

  if (widths[0] > 0)
    sizes[0].h = MAX (widths[0], rounded_outline->corner[0].height);
  else
    sizes[0].h = 0;

  /* Top right */
  if (widths[1] > 0)
    sizes[1].w = MAX (widths[1], rounded_outline->corner[1].width);
  else
    sizes[1].w = 0;

  if (widths[0] > 0)
    sizes[1].h = MAX (widths[0], rounded_outline->corner[1].height);
  else
    sizes[1].h = 0;

  /* Bottom right */
  if (widths[1] > 0)
    sizes[2].w = MAX (widths[1], rounded_outline->corner[2].width);
  else
    sizes[2].w = 0;

  if (widths[2] > 0)
    sizes[2].h = MAX (widths[2], rounded_outline->corner[2].height);
  else
    sizes[2].h = 0;


  /* Bottom left */
  if (widths[3] > 0)
    sizes[3].w = MAX (widths[3], rounded_outline->corner[3].width);
  else
    sizes[3].w = 0;

  if (widths[2] > 0)
    sizes[3].h = MAX (widths[2], rounded_outline->corner[3].height);
  else
    sizes[3].h = 0;

  {
    float min_x = job->offset_x + node->bounds.origin.x;
    float min_y = job->offset_y + node->bounds.origin.y;
    float max_x = min_x + node->bounds.size.width;
    float max_y = min_y + node->bounds.size.height;
    const GskNglDrawVertex side_data[4][6] = {
      /* Top */
      {
        { { min_x,              min_y              }, { 0, 1 }, { c[0].red, c[0].green, c[0].blue, c[0].alpha } }, /* Upper left */
        { { min_x + sizes[0].w, min_y + sizes[0].h }, { 0, 0 }, { c[0].red, c[0].green, c[0].blue, c[0].alpha } }, /* Lower left */
        { { max_x,              min_y              }, { 1, 1 }, { c[0].red, c[0].green, c[0].blue, c[0].alpha } }, /* Upper right */

        { { max_x - sizes[1].w, min_y + sizes[1].h }, { 1, 0 }, { c[0].red, c[0].green, c[0].blue, c[0].alpha } }, /* Lower right */
        { { min_x + sizes[0].w, min_y + sizes[0].h }, { 0, 0 }, { c[0].red, c[0].green, c[0].blue, c[0].alpha } }, /* Lower left */
        { { max_x,              min_y              }, { 1, 1 }, { c[0].red, c[0].green, c[0].blue, c[0].alpha } }, /* Upper right */
      },
      /* Right */
      {
        { { max_x - sizes[1].w, min_y + sizes[1].h }, { 0, 1 }, { c[1].red, c[1].green, c[1].blue, c[1].alpha } }, /* Upper left */
        { { max_x - sizes[2].w, max_y - sizes[2].h }, { 0, 0 }, { c[1].red, c[1].green, c[1].blue, c[1].alpha } }, /* Lower left */
        { { max_x,              min_y              }, { 1, 1 }, { c[1].red, c[1].green, c[1].blue, c[1].alpha } }, /* Upper right */

        { { max_x,              max_y              }, { 1, 0 }, { c[1].red, c[1].green, c[1].blue, c[1].alpha } }, /* Lower right */
        { { max_x - sizes[2].w, max_y - sizes[2].h }, { 0, 0 }, { c[1].red, c[1].green, c[1].blue, c[1].alpha } }, /* Lower left */
        { { max_x,              min_y              }, { 1, 1 }, { c[1].red, c[1].green, c[1].blue, c[1].alpha } }, /* Upper right */
      },
      /* Bottom */
      {
        { { min_x + sizes[3].w, max_y - sizes[3].h }, { 0, 1 }, { c[2].red, c[2].green, c[2].blue, c[2].alpha } }, /* Upper left */
        { { min_x,              max_y              }, { 0, 0 }, { c[2].red, c[2].green, c[2].blue, c[2].alpha } }, /* Lower left */
        { { max_x - sizes[2].w, max_y - sizes[2].h }, { 1, 1 }, { c[2].red, c[2].green, c[2].blue, c[2].alpha } }, /* Upper right */

        { { max_x,              max_y              }, { 1, 0 }, { c[2].red, c[2].green, c[2].blue, c[2].alpha } }, /* Lower right */
        { { min_x            ,  max_y              }, { 0, 0 }, { c[2].red, c[2].green, c[2].blue, c[2].alpha } }, /* Lower left */
        { { max_x - sizes[2].w, max_y - sizes[2].h }, { 1, 1 }, { c[2].red, c[2].green, c[2].blue, c[2].alpha } }, /* Upper right */
      },
      /* Left */
      {
        { { min_x,              min_y              }, { 0, 1 }, { c[3].red, c[3].green, c[3].blue, c[3].alpha } }, /* Upper left */
        { { min_x,              max_y              }, { 0, 0 }, { c[3].red, c[3].green, c[3].blue, c[3].alpha } }, /* Lower left */
        { { min_x + sizes[0].w, min_y + sizes[0].h }, { 1, 1 }, { c[3].red, c[3].green, c[3].blue, c[3].alpha } }, /* Upper right */

        { { min_x + sizes[3].w, max_y - sizes[3].h }, { 1, 0 }, { c[3].red, c[3].green, c[3].blue, c[3].alpha } }, /* Lower right */
        { { min_x,              max_y              }, { 0, 0 }, { c[3].red, c[3].green, c[3].blue, c[3].alpha } }, /* Lower left */
        { { min_x + sizes[0].w, min_y + sizes[0].h }, { 1, 1 }, { c[3].red, c[3].green, c[3].blue, c[3].alpha } }, /* Upper right */
      }
    };
    GskRoundedRect outline;
    GskNglProgram *program;

    /* Prepare outline */
    gsk_ngl_render_job_transform_rounded_rect (job, rounded_outline, &outline);

    program = CHOOSE_PROGRAM (job, border);

    gsk_ngl_program_set_uniform4fv (program,
                                    UNIFORM_BORDER_WIDTHS, 0,
                                    1,
                                    widths);
    gsk_ngl_program_set_uniform_rounded_rect (program,
                                              UNIFORM_BORDER_OUTLINE_RECT, 0,
                                              &outline);

    for (guint i = 0; i < 4; i++)
      {
        GskNglDrawVertex *vertices;

        if (widths[i] <= 0)
          continue;

        gsk_ngl_render_job_begin_draw (job, program);
        vertices = gsk_ngl_command_queue_add_vertices (job->command_queue);
        memcpy (vertices, side_data[i], sizeof (GskNglDrawVertex) * GSK_NGL_N_VERTICES);
        gsk_ngl_render_job_end_draw (job);
      }
  }
}

/* Returns TRUE if applying @transform to @bounds
 * yields an axis-aligned rectangle
 */
static gboolean
result_is_axis_aligned (GskTransform          *transform,
                        const graphene_rect_t *bounds)
{
  graphene_matrix_t m;
  graphene_quad_t q;
  graphene_rect_t b;
  graphene_point_t b1, b2;
  const graphene_point_t *p;

  gsk_transform_to_matrix (transform, &m);
  gsk_matrix_transform_rect (&m, bounds, &q);
  graphene_quad_bounds (&q, &b);
  graphene_rect_get_top_left (&b, &b1);
  graphene_rect_get_bottom_right (&b, &b2);

  for (guint i = 0; i < 4; i++)
    {
      p = graphene_quad_get_point (&q, i);
      if (fabs (p->x - b1.x) > FLT_EPSILON && fabs (p->x - b2.x) > FLT_EPSILON)
        return FALSE;
      if (fabs (p->y - b1.y) > FLT_EPSILON && fabs (p->y - b2.y) > FLT_EPSILON)
        return FALSE;
    }

  return TRUE;
}

static inline void
gsk_ngl_render_job_visit_transform_node (GskNglRenderJob     *job,
                                         const GskRenderNode *node)
{
  GskTransform *transform = gsk_transform_node_get_transform (node);
  const GskTransformCategory category = gsk_transform_get_category (transform);
  const GskRenderNode *child = gsk_transform_node_get_child (node);

  switch (category)
    {
    case GSK_TRANSFORM_CATEGORY_IDENTITY:
      gsk_ngl_render_job_visit_node (job, child);
    break;

    case GSK_TRANSFORM_CATEGORY_2D_TRANSLATE:
      {
        float dx, dy;

        gsk_transform_to_translate (transform, &dx, &dy);
        gsk_ngl_render_job_offset (job, dx, dy);
        gsk_ngl_render_job_visit_node (job, child);
        gsk_ngl_render_job_offset (job, -dx, -dy);
      }
    break;

    case GSK_TRANSFORM_CATEGORY_2D_AFFINE:
      {
        gsk_ngl_render_job_push_modelview (job, transform);
        gsk_ngl_render_job_visit_node (job, child);
        gsk_ngl_render_job_pop_modelview (job);
      }
    break;

    case GSK_TRANSFORM_CATEGORY_2D:
    case GSK_TRANSFORM_CATEGORY_3D:
    case GSK_TRANSFORM_CATEGORY_ANY:
    case GSK_TRANSFORM_CATEGORY_UNKNOWN:
      if (node_supports_transform (child))
        {
          gsk_ngl_render_job_push_modelview (job, transform);
          gsk_ngl_render_job_visit_node (job, child);
          gsk_ngl_render_job_pop_modelview (job);
        }
      else
        {
          GskNglRenderOffscreen offscreen = {0};

          offscreen.bounds = &child->bounds;
          offscreen.reset_clip = TRUE;

          if (!result_is_axis_aligned (transform, &child->bounds))
            offscreen.linear_filter = TRUE;

          if (gsk_ngl_render_job_visit_node_with_offscreen (job, child, &offscreen))
            {
              /* For non-trivial transforms, we draw everything on a texture and then
               * draw the texture transformed. */
              /* TODO: We should compute a modelview containing only the "non-trivial"
               *       part (e.g. the rotation) and use that. We want to keep the scale
               *       for the texture.
               */
              gsk_ngl_render_job_push_modelview (job, transform);

              gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blit));
              gsk_ngl_program_set_uniform_texture (job->current_program,
                                                   UNIFORM_SHARED_SOURCE, 0,
                                                   GL_TEXTURE_2D,
                                                   GL_TEXTURE0,
                                                   offscreen.texture_id);
              gsk_ngl_render_job_load_vertices_from_offscreen (job, &child->bounds, &offscreen);
              gsk_ngl_render_job_end_draw (job);

              gsk_ngl_render_job_pop_modelview (job);
            }
        }
    break;

    default:
      g_assert_not_reached ();
    }
}

static inline void
gsk_ngl_render_job_visit_unblurred_inset_shadow_node (GskNglRenderJob     *job,
                                                      const GskRenderNode *node)
{
  const GskRoundedRect *outline = gsk_inset_shadow_node_get_outline (node);
  GskRoundedRect transformed_outline;

  gsk_ngl_render_job_transform_rounded_rect (job, outline, &transformed_outline);

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, inset_shadow));
  gsk_ngl_render_job_set_color (job, gsk_inset_shadow_node_get_color (node));
  gsk_ngl_program_set_uniform_rounded_rect (job->current_program,
                                            UNIFORM_INSET_SHADOW_OUTLINE_RECT, 0,
                                            &transformed_outline);
  gsk_ngl_program_set_uniform1f (job->current_program,
                                 UNIFORM_INSET_SHADOW_SPREAD, 0,
                                 gsk_inset_shadow_node_get_spread (node));
  gsk_ngl_program_set_uniform2f (job->current_program,
                                 UNIFORM_INSET_SHADOW_OFFSET, 0,
                                 gsk_inset_shadow_node_get_dx (node),
                                 gsk_inset_shadow_node_get_dy (node));
  gsk_ngl_render_job_draw_rect (job, &node->bounds);
  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_blurred_inset_shadow_node (GskNglRenderJob     *job,
                                                    const GskRenderNode *node)
{
  const GskRoundedRect *node_outline = gsk_inset_shadow_node_get_outline (node);
  float blur_radius = gsk_inset_shadow_node_get_blur_radius (node);
  float offset_x = gsk_inset_shadow_node_get_dx (node);
  float offset_y = gsk_inset_shadow_node_get_dy (node);
  float scale_x = job->scale_x;
  float scale_y = job->scale_y;
  float blur_extra = blur_radius * 2.0; /* 2.0 = shader radius_multiplier */
  float half_blur_extra = blur_radius;
  float texture_width;
  float texture_height;
  int blurred_texture_id;
  GskTextureKey key;
  GskNglRenderOffscreen offscreen = {0};

  g_assert (blur_radius > 0);

  texture_width = ceilf ((node_outline->bounds.size.width + blur_extra) * scale_x);
  texture_height = ceilf ((node_outline->bounds.size.height + blur_extra) * scale_y);

  key.pointer = node;
  key.pointer_is_child = FALSE;
  key.scale_x = scale_x;
  key.scale_y = scale_y;
  key.filter = GL_NEAREST;

  blurred_texture_id = gsk_ngl_driver_lookup_texture (job->driver, &key);

  if (blurred_texture_id == 0)
    {
      float spread = gsk_inset_shadow_node_get_spread (node) + half_blur_extra;
      GskRoundedRect transformed_outline;
      GskRoundedRect outline_to_blur;
      GskNglRenderTarget *render_target;
      graphene_matrix_t prev_projection;
      graphene_rect_t prev_viewport;
      guint prev_fbo;

      /* TODO: In the following code, we have to be careful about where we apply the scale.
       * We're manually scaling stuff (e.g. the outline) so we can later use texture_width
       * and texture_height (which are already scaled) as the geometry and keep the modelview
       * at a scale of 1. That's kinda complicated though... */

      /* Outline of what we actually want to blur later.
       * Spread grows inside, so we don't need to account for that. But the blur will need
       * to read outside of the inset shadow, so we need to draw some color in there. */
      outline_to_blur = *node_outline;
      gsk_rounded_rect_shrink (&outline_to_blur,
                               -half_blur_extra,
                               -half_blur_extra,
                               -half_blur_extra,
                               -half_blur_extra);

      /* Fit to our texture */
      outline_to_blur.bounds.origin.x = 0;
      outline_to_blur.bounds.origin.y = 0;
      outline_to_blur.bounds.size.width *= scale_x;
      outline_to_blur.bounds.size.height *= scale_y;

      for (guint i = 0; i < 4; i ++)
        {
          outline_to_blur.corner[i].width *= scale_x;
          outline_to_blur.corner[i].height *= scale_y;
        }

      if (!gsk_ngl_driver_create_render_target (job->driver,
                                                 texture_width, texture_height,
                                                 GL_NEAREST, GL_NEAREST,
                                                 &render_target))
        g_assert_not_reached ();

      gsk_ngl_render_job_set_viewport_for_size (job, texture_width, texture_height, &prev_viewport);
      gsk_ngl_render_job_set_projection_for_size (job, texture_width, texture_height, &prev_projection);
      gsk_ngl_render_job_set_modelview (job, NULL);
      gsk_ngl_render_job_push_clip (job, &GSK_ROUNDED_RECT_INIT (0, 0, texture_width, texture_height));

      prev_fbo = gsk_ngl_command_queue_bind_framebuffer (job->command_queue, render_target->framebuffer_id);
      gsk_ngl_command_queue_clear (job->command_queue, 0, &job->viewport);

      gsk_ngl_render_job_transform_rounded_rect (job, &outline_to_blur, &transformed_outline);

      /* Actual inset shadow outline drawing */
      gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, inset_shadow));
      gsk_ngl_render_job_set_color (job, gsk_inset_shadow_node_get_color (node));
      gsk_ngl_program_set_uniform_rounded_rect (job->current_program,
                                                UNIFORM_INSET_SHADOW_OUTLINE_RECT, 0,
                                                &transformed_outline);
      gsk_ngl_program_set_uniform1f (job->current_program,
                                     UNIFORM_INSET_SHADOW_SPREAD, 0,
                                     spread * MAX (scale_x, scale_y));
      gsk_ngl_program_set_uniform2f (job->current_program,
                                     UNIFORM_INSET_SHADOW_OFFSET, 0,
                                     offset_x * scale_x,
                                     offset_y * scale_y);
      gsk_ngl_render_job_draw (job, 0, 0, texture_width, texture_height);
      gsk_ngl_render_job_end_draw (job);

      gsk_ngl_render_job_pop_modelview (job);
      gsk_ngl_render_job_pop_clip (job);
      gsk_ngl_render_job_set_projection (job, &prev_projection);
      gsk_ngl_render_job_set_viewport (job, &prev_viewport, NULL);
      gsk_ngl_command_queue_bind_framebuffer (job->command_queue, prev_fbo);

      offscreen.texture_id = render_target->texture_id;
      init_full_texture_region (&offscreen);

      blurred_texture_id = blur_offscreen (job,
                                           &offscreen,
                                           texture_width,
                                           texture_height,
                                           blur_radius * scale_x,
                                           blur_radius * scale_y);

      gsk_ngl_driver_release_render_target (job->driver, render_target, TRUE);

      gsk_ngl_driver_cache_texture (job->driver, &key, blurred_texture_id);
    }

  g_assert (blurred_texture_id != 0);

  /* Blur the rendered unblurred inset shadow */
  /* Use a clip to cut away the unwanted parts outside of the original outline */
  {
    const gboolean needs_clip = !gsk_rounded_rect_is_rectilinear (node_outline);
    const float tx1 = half_blur_extra * scale_x / texture_width;
    const float tx2 = 1.0 - tx1;
    const float ty1 = half_blur_extra * scale_y / texture_height;
    const float ty2 = 1.0 - ty1;

    if (needs_clip)
      {
        GskRoundedRect node_clip;

        gsk_ngl_render_job_transform_bounds (job, &node_outline->bounds, &node_clip.bounds);

        for (guint i = 0; i < 4; i ++)
          {
            node_clip.corner[i].width = node_outline->corner[i].width * scale_x;
            node_clip.corner[i].height = node_outline->corner[i].height * scale_y;
          }

        gsk_ngl_render_job_push_clip (job, &node_clip);
      }

    offscreen.was_offscreen = TRUE;
    offscreen.area.x = tx1;
    offscreen.area.y = ty1;
    offscreen.area.x2 = tx2;
    offscreen.area.y2 = ty2;

    gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blit));
    gsk_ngl_program_set_uniform_texture (job->current_program,
                                         UNIFORM_SHARED_SOURCE, 0,
                                         GL_TEXTURE_2D,
                                         GL_TEXTURE0,
                                         blurred_texture_id);
    gsk_ngl_render_job_load_vertices_from_offscreen (job, &node->bounds, &offscreen);
    gsk_ngl_render_job_end_draw (job);

    if (needs_clip)
      gsk_ngl_render_job_pop_clip (job);
  }
}

static inline void
gsk_ngl_render_job_visit_unblurred_outset_shadow_node (GskNglRenderJob     *job,
                                                       const GskRenderNode *node)
{
  const GskRoundedRect *outline = gsk_outset_shadow_node_get_outline (node);
  GskRoundedRect transformed_outline;
  float x = node->bounds.origin.x;
  float y = node->bounds.origin.y;
  float w = node->bounds.size.width;
  float h = node->bounds.size.height;
  float spread = gsk_outset_shadow_node_get_spread (node);
  float dx = gsk_outset_shadow_node_get_dx (node);
  float dy = gsk_outset_shadow_node_get_dy (node);
  const float edge_sizes[] = { // Top, right, bottom, left
    spread - dy, spread + dx, spread + dy, spread - dx
  };
  const float corner_sizes[][2] = { // top left, top right, bottom right, bottom left
    { outline->corner[0].width + spread - dx, outline->corner[0].height + spread - dy },
    { outline->corner[1].width + spread + dx, outline->corner[1].height + spread - dy },
    { outline->corner[2].width + spread + dx, outline->corner[2].height + spread + dy },
    { outline->corner[3].width + spread - dx, outline->corner[3].height + spread + dy },
  };

  gsk_ngl_render_job_transform_rounded_rect (job, outline, &transformed_outline);

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, unblurred_outset_shadow));
  gsk_ngl_render_job_set_color (job, gsk_outset_shadow_node_get_color (node));
  gsk_ngl_program_set_uniform_rounded_rect (job->current_program,
                                            UNIFORM_UNBLURRED_OUTSET_SHADOW_OUTLINE_RECT, 0,
                                            &transformed_outline);
  gsk_ngl_program_set_uniform1f (job->current_program,
                                 UNIFORM_UNBLURRED_OUTSET_SHADOW_SPREAD, 0,
                                 spread);
  gsk_ngl_program_set_uniform2f (job->current_program,
                                 UNIFORM_UNBLURRED_OUTSET_SHADOW_OFFSET, 0,
                                 dx, dy);

  /* Corners... */
  if (corner_sizes[0][0] > 0 && corner_sizes[0][1] > 0) /* Top left */
    gsk_ngl_render_job_draw (job,
                             x, y,
                             corner_sizes[0][0], corner_sizes[0][1]);
  if (corner_sizes[1][0] > 0 && corner_sizes[1][1] > 0) /* Top right */
    gsk_ngl_render_job_draw (job,
                             x + w - corner_sizes[1][0], y,
                             corner_sizes[1][0], corner_sizes[1][1]);
  if (corner_sizes[2][0] > 0 && corner_sizes[2][1] > 0) /* Bottom right */
    gsk_ngl_render_job_draw (job,
                             x + w - corner_sizes[2][0], y + h - corner_sizes[2][1],
                             corner_sizes[2][0], corner_sizes[2][1]);
  if (corner_sizes[3][0] > 0 && corner_sizes[3][1] > 0) /* Bottom left */
    gsk_ngl_render_job_draw (job,
                             x, y + h - corner_sizes[3][1],
                             corner_sizes[3][0], corner_sizes[3][1]);
  /* Edges... */;
  if (edge_sizes[0] > 0) /* Top */
    gsk_ngl_render_job_draw (job,
                             x + corner_sizes[0][0], y,
                             w - corner_sizes[0][0] - corner_sizes[1][0], edge_sizes[0]);
  if (edge_sizes[1] > 0) /* Right */
    gsk_ngl_render_job_draw (job,
                             x + w - edge_sizes[1], y + corner_sizes[1][1],
                             edge_sizes[1], h - corner_sizes[1][1] - corner_sizes[2][1]);
  if (edge_sizes[2] > 0) /* Bottom */
    gsk_ngl_render_job_draw (job,
                             x + corner_sizes[3][0], y + h - edge_sizes[2],
                             w - corner_sizes[3][0] - corner_sizes[2][0], edge_sizes[2]);
  if (edge_sizes[3] > 0) /* Left */
    gsk_ngl_render_job_draw (job,
                             x, y + corner_sizes[0][1],
                             edge_sizes[3], h - corner_sizes[0][1] - corner_sizes[3][1]);

  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_blurred_outset_shadow_node (GskNglRenderJob     *job,
                                                     const GskRenderNode *node)
{
  static const GdkRGBA white = { 1, 1, 1, 1 };

  const GskRoundedRect *outline = gsk_outset_shadow_node_get_outline (node);
  const GdkRGBA *color = gsk_outset_shadow_node_get_color (node);
  float scale_x = job->scale_x;
  float scale_y = job->scale_y;
  float blur_radius = gsk_outset_shadow_node_get_blur_radius (node);
  float blur_extra = blur_radius * 2.0f; /* 2.0 = shader radius_multiplier */
  float half_blur_extra = blur_extra / 2.0f;
  int extra_blur_pixels = ceilf (half_blur_extra * scale_x);
  float spread = gsk_outset_shadow_node_get_spread (node);
  float dx = gsk_outset_shadow_node_get_dx (node);
  float dy = gsk_outset_shadow_node_get_dy (node);
  GskRoundedRect scaled_outline;
  GskRoundedRect transformed_outline;
  GskNglRenderOffscreen offscreen = {0};
  int texture_width, texture_height;
  int blurred_texture_id;
  int cached_tid;
  gboolean do_slicing;

  /* scaled_outline is the minimal outline we need to draw the given drop shadow,
   * enlarged by the spread and offset by the blur radius. */
  scaled_outline = *outline;

  if (outline->bounds.size.width < blur_extra ||
      outline->bounds.size.height < blur_extra)
    {
      do_slicing = FALSE;
      gsk_rounded_rect_shrink (&scaled_outline, -spread, -spread, -spread, -spread);
    }
  else
    {
      /* Shrink our outline to the minimum size that can still hold all the border radii */
      gsk_rounded_rect_shrink_to_minimum (&scaled_outline);
      /* Increase by the spread */
      gsk_rounded_rect_shrink (&scaled_outline, -spread, -spread, -spread, -spread);
      /* Grow bounds but don't grow corners */
      graphene_rect_inset (&scaled_outline.bounds, - blur_extra / 2.0, - blur_extra / 2.0);
      /* For the center part, we add a few pixels */
      scaled_outline.bounds.size.width += SHADOW_EXTRA_SIZE;
      scaled_outline.bounds.size.height += SHADOW_EXTRA_SIZE;

      do_slicing = TRUE;
    }

  texture_width = (int)ceil ((scaled_outline.bounds.size.width + blur_extra) * scale_x);
  texture_height = (int)ceil ((scaled_outline.bounds.size.height + blur_extra) * scale_y);

  scaled_outline.bounds.origin.x = extra_blur_pixels;
  scaled_outline.bounds.origin.y = extra_blur_pixels;
  scaled_outline.bounds.size.width = texture_width - (extra_blur_pixels * 2);
  scaled_outline.bounds.size.height = texture_height - (extra_blur_pixels * 2);

  for (guint i = 0; i < G_N_ELEMENTS (scaled_outline.corner); i++)
    {
      scaled_outline.corner[i].width *= scale_x;
      scaled_outline.corner[i].height *= scale_y;
    }

  cached_tid = gsk_ngl_shadow_library_lookup (job->driver->shadows, &scaled_outline, blur_radius);

  if (cached_tid == 0)
    {
      GdkGLContext *context = job->command_queue->context;
      GskNglRenderTarget *render_target;
      graphene_matrix_t prev_projection;
      graphene_rect_t prev_viewport;
      guint prev_fbo;

      gsk_ngl_driver_create_render_target (job->driver,
                                           texture_width, texture_height,
                                           GL_NEAREST, GL_NEAREST,
                                           &render_target);

      if (gdk_gl_context_has_debug (context))
        {
          gdk_gl_context_label_object_printf (context,
                                              GL_TEXTURE,
                                              render_target->texture_id,
                                              "Outset Shadow Temp %d",
                                              render_target->texture_id);
          gdk_gl_context_label_object_printf  (context,
                                               GL_FRAMEBUFFER,
                                               render_target->framebuffer_id,
                                               "Outset Shadow FB Temp %d",
                                               render_target->framebuffer_id);
        }

      /* Change state for offscreen */
      gsk_ngl_render_job_set_projection_for_size (job, texture_width, texture_height, &prev_projection);
      gsk_ngl_render_job_set_viewport_for_size (job, texture_width, texture_height, &prev_viewport);
      gsk_ngl_render_job_set_modelview (job, NULL);
      gsk_ngl_render_job_push_clip (job, &scaled_outline);

      /* Bind render target and clear it */
      prev_fbo = gsk_ngl_command_queue_bind_framebuffer (job->command_queue, render_target->framebuffer_id);
      gsk_ngl_command_queue_clear (job->command_queue, 0, &job->viewport);

      /* Draw the outline using color program */
      gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, color));
      gsk_ngl_render_job_set_color (job, &white);
      gsk_ngl_render_job_draw (job, 0, 0, texture_width, texture_height);
      gsk_ngl_render_job_end_draw (job);

      /* Reset state from offscreen */
      gsk_ngl_render_job_pop_clip (job);
      gsk_ngl_render_job_pop_modelview (job);
      gsk_ngl_render_job_set_viewport (job, &prev_viewport, NULL);
      gsk_ngl_render_job_set_projection (job, &prev_projection);

      /* Now blur the outline */
      init_full_texture_region (&offscreen);
      offscreen.texture_id = gsk_ngl_driver_release_render_target (job->driver, render_target, FALSE);
      blurred_texture_id = blur_offscreen (job,
                                           &offscreen,
                                           texture_width,
                                           texture_height,
                                           blur_radius * scale_x,
                                           blur_radius * scale_y);

      gsk_ngl_shadow_library_insert (job->driver->shadows,
                                     &scaled_outline,
                                     blur_radius,
                                     blurred_texture_id);

      gsk_ngl_command_queue_bind_framebuffer (job->command_queue, prev_fbo);
    }
  else
    {
      blurred_texture_id = cached_tid;
    }

  gsk_ngl_render_job_transform_rounded_rect (job, outline, &transformed_outline);

  if (!do_slicing)
    {
      float min_x = floorf (outline->bounds.origin.x - spread - half_blur_extra + dx);
      float min_y = floorf (outline->bounds.origin.y - spread - half_blur_extra + dy);

      offscreen.was_offscreen = FALSE;
      offscreen.texture_id = blurred_texture_id;
      init_full_texture_region (&offscreen);

      gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, outset_shadow));
      gsk_ngl_render_job_set_color (job, color);
      gsk_ngl_program_set_uniform_texture (job->current_program,
                                           UNIFORM_SHARED_SOURCE, 0,
                                           GL_TEXTURE_2D,
                                           GL_TEXTURE0,
                                           blurred_texture_id);
      gsk_ngl_program_set_uniform_rounded_rect (job->current_program,
                                                UNIFORM_OUTSET_SHADOW_OUTLINE_RECT, 0,
                                                &transformed_outline);
      gsk_ngl_render_job_load_vertices_from_offscreen (job,
                                                       &GRAPHENE_RECT_INIT (min_x,
                                                                            min_y,
                                                                            texture_width / scale_x,
                                                                            texture_height / scale_y),
                                                       &offscreen);
      gsk_ngl_render_job_end_draw (job);

      return;
    }

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, outset_shadow));
  gsk_ngl_render_job_set_color (job, color);
  gsk_ngl_program_set_uniform_texture (job->current_program,
                                       UNIFORM_SHARED_SOURCE, 0,
                                       GL_TEXTURE_2D,
                                       GL_TEXTURE0,
                                       blurred_texture_id);
  gsk_ngl_program_set_uniform_rounded_rect (job->current_program,
                                            UNIFORM_OUTSET_SHADOW_OUTLINE_RECT, 0,
                                            &transformed_outline);

  {
    float min_x = floorf (outline->bounds.origin.x - spread - half_blur_extra + dx);
    float min_y = floorf (outline->bounds.origin.y - spread - half_blur_extra + dy);
    float max_x = ceilf (outline->bounds.origin.x + outline->bounds.size.width +
                         half_blur_extra + dx + spread);
    float max_y = ceilf (outline->bounds.origin.y + outline->bounds.size.height +
                         half_blur_extra + dy + spread);
    const GskNglTextureNineSlice *slices;
    GskNglTexture *texture;

    texture = gsk_ngl_driver_get_texture_by_id (job->driver, blurred_texture_id);
    slices = gsk_ngl_texture_get_nine_slice (texture, &scaled_outline, extra_blur_pixels);

    offscreen.was_offscreen = TRUE;

    /* Our texture coordinates MUST be scaled, while the actual vertex coords
     * MUST NOT be scaled. */

    /* Top left */
    if (nine_slice_is_visible (&slices[NINE_SLICE_TOP_LEFT]))
      {
        memcpy (&offscreen.area, &slices[NINE_SLICE_TOP_LEFT].area, sizeof offscreen.area);
        gsk_ngl_render_job_load_vertices_from_offscreen (job,
                                                         &GRAPHENE_RECT_INIT (min_x, min_y,
                                                                              slices[NINE_SLICE_TOP_LEFT].rect.width / scale_x,
                                                                              slices[NINE_SLICE_TOP_LEFT].rect.height / scale_y),
                                                         &offscreen);
      }

    /* Top center */
    if (nine_slice_is_visible (&slices[NINE_SLICE_TOP_CENTER]))
    {
      memcpy (&offscreen.area, &slices[NINE_SLICE_TOP_CENTER].area, sizeof offscreen.area);
      float width = (max_x - min_x) - (slices[NINE_SLICE_TOP_LEFT].rect.width / scale_x +
                                       slices[NINE_SLICE_TOP_RIGHT].rect.width / scale_x);
      gsk_ngl_render_job_load_vertices_from_offscreen (job,
                                                       &GRAPHENE_RECT_INIT (min_x + (slices[NINE_SLICE_TOP_LEFT].rect.width / scale_x),
                                                                            min_y,
                                                                            width,
                                                                            slices[NINE_SLICE_TOP_CENTER].rect.height / scale_y),
                                                       &offscreen);
    }

    /* Top right */
    if (nine_slice_is_visible (&slices[NINE_SLICE_TOP_RIGHT]))
    {
      memcpy (&offscreen.area, &slices[NINE_SLICE_TOP_RIGHT].area, sizeof offscreen.area);
      gsk_ngl_render_job_load_vertices_from_offscreen (job,
                                                       &GRAPHENE_RECT_INIT (max_x - (slices[NINE_SLICE_TOP_RIGHT].rect.width / scale_x),
                                                                            min_y,
                                                                            slices[NINE_SLICE_TOP_RIGHT].rect.width / scale_x,
                                                                            slices[NINE_SLICE_TOP_RIGHT].rect.height / scale_y),
                                                       &offscreen);
    }

    /* Bottom right */
    if (nine_slice_is_visible (&slices[NINE_SLICE_BOTTOM_RIGHT]))
    {
      memcpy (&offscreen.area, &slices[NINE_SLICE_BOTTOM_RIGHT].area, sizeof offscreen.area);
      gsk_ngl_render_job_load_vertices_from_offscreen (job,
                                                       &GRAPHENE_RECT_INIT (max_x - (slices[NINE_SLICE_BOTTOM_RIGHT].rect.width / scale_x),
                                                                            max_y - (slices[NINE_SLICE_BOTTOM_RIGHT].rect.height / scale_y),
                                                                            slices[NINE_SLICE_BOTTOM_RIGHT].rect.width / scale_x,
                                                                            slices[NINE_SLICE_BOTTOM_RIGHT].rect.height / scale_y),
                                                       &offscreen);
    }

    /* Bottom left */
    if (nine_slice_is_visible (&slices[NINE_SLICE_BOTTOM_LEFT]))
      {
        memcpy (&offscreen.area, &slices[NINE_SLICE_BOTTOM_LEFT].area, sizeof offscreen.area);
        gsk_ngl_render_job_load_vertices_from_offscreen (job,
                                                         &GRAPHENE_RECT_INIT (min_x,
                                                                              max_y - (slices[NINE_SLICE_BOTTOM_LEFT].rect.height / scale_y),
                                                                              slices[NINE_SLICE_BOTTOM_LEFT].rect.width / scale_x,
                                                                              slices[NINE_SLICE_BOTTOM_LEFT].rect.height / scale_y),
                                                         &offscreen);
      }

    /* Left side */
    if (nine_slice_is_visible (&slices[NINE_SLICE_LEFT_CENTER]))
      {
        memcpy (&offscreen.area, &slices[NINE_SLICE_LEFT_CENTER].area, sizeof offscreen.area);
        float height = (max_y - min_y) - (slices[NINE_SLICE_TOP_LEFT].rect.height / scale_y +
                                                slices[NINE_SLICE_BOTTOM_LEFT].rect.height / scale_y);
        gsk_ngl_render_job_load_vertices_from_offscreen (job,
                                                         &GRAPHENE_RECT_INIT (min_x,
                                                                              min_y + (slices[NINE_SLICE_TOP_LEFT].rect.height / scale_y),
                                                                              slices[NINE_SLICE_LEFT_CENTER].rect.width / scale_x,
                                                                              height),
                                                         &offscreen);
      }

    /* Right side */
    if (nine_slice_is_visible (&slices[NINE_SLICE_RIGHT_CENTER]))
      {
        memcpy (&offscreen.area, &slices[NINE_SLICE_RIGHT_CENTER].area, sizeof offscreen.area);
        float height = (max_y - min_y) - (slices[NINE_SLICE_TOP_RIGHT].rect.height / scale_y +
                                          slices[NINE_SLICE_BOTTOM_RIGHT].rect.height / scale_y);
        gsk_ngl_render_job_load_vertices_from_offscreen (job,
                                                         &GRAPHENE_RECT_INIT (max_x - (slices[NINE_SLICE_RIGHT_CENTER].rect.width / scale_x),
                                                                              min_y + (slices[NINE_SLICE_TOP_LEFT].rect.height / scale_y),
                                                                              slices[NINE_SLICE_RIGHT_CENTER].rect.width / scale_x,
                                                                              height),
                                                         &offscreen);
      }

    /* Bottom side */
    if (nine_slice_is_visible (&slices[NINE_SLICE_BOTTOM_CENTER]))
      {
        memcpy (&offscreen.area, &slices[NINE_SLICE_BOTTOM_CENTER].area, sizeof offscreen.area);
        float width = (max_x - min_x) - (slices[NINE_SLICE_BOTTOM_LEFT].rect.width / scale_x +
                                         slices[NINE_SLICE_BOTTOM_RIGHT].rect.width / scale_x);
        gsk_ngl_render_job_load_vertices_from_offscreen (job,
                                                         &GRAPHENE_RECT_INIT (min_x + (slices[NINE_SLICE_BOTTOM_LEFT].rect.width / scale_x),
                                                                              max_y - (slices[NINE_SLICE_BOTTOM_CENTER].rect.height / scale_y),
                                                                              width,
                                                                              slices[NINE_SLICE_BOTTOM_CENTER].rect.height / scale_y),
                                                         &offscreen);
      }

    /* Middle */
    if (nine_slice_is_visible (&slices[NINE_SLICE_CENTER]))
      {
        memcpy (&offscreen.area, &slices[NINE_SLICE_CENTER].area, sizeof offscreen.area);
        float width = (max_x - min_x) - (slices[NINE_SLICE_LEFT_CENTER].rect.width / scale_x +
                                         slices[NINE_SLICE_RIGHT_CENTER].rect.width / scale_x);
        float height = (max_y - min_y) - (slices[NINE_SLICE_TOP_CENTER].rect.height / scale_y +
                                          slices[NINE_SLICE_BOTTOM_CENTER].rect.height / scale_y);
        gsk_ngl_render_job_load_vertices_from_offscreen (job,
                                                         &GRAPHENE_RECT_INIT (min_x + (slices[NINE_SLICE_LEFT_CENTER].rect.width / scale_x),
                                                                              min_y + (slices[NINE_SLICE_TOP_CENTER].rect.height / scale_y),
                                                                              width, height),
                                                         &offscreen);
      }
  }

  gsk_ngl_render_job_end_draw (job);
}

static inline gboolean G_GNUC_PURE
equal_texture_nodes (const GskRenderNode *node1,
                     const GskRenderNode *node2)
{
  if (gsk_render_node_get_node_type (node1) != GSK_TEXTURE_NODE ||
      gsk_render_node_get_node_type (node2) != GSK_TEXTURE_NODE)
    return FALSE;

  if (gsk_texture_node_get_texture (node1) !=
      gsk_texture_node_get_texture (node2))
    return FALSE;

  return graphene_rect_equal (&node1->bounds, &node2->bounds);
}

static inline void
gsk_ngl_render_job_visit_cross_fade_node (GskNglRenderJob     *job,
                                          const GskRenderNode *node)
{
  const GskRenderNode *start_node = gsk_cross_fade_node_get_start_child (node);
  const GskRenderNode *end_node = gsk_cross_fade_node_get_end_child (node);
  float progress = gsk_cross_fade_node_get_progress (node);
  GskNglRenderOffscreen offscreen_start = {0};
  GskNglRenderOffscreen offscreen_end = {0};

  g_assert (progress > 0.0);
  g_assert (progress < 1.0);

  offscreen_start.force_offscreen = TRUE;
  offscreen_start.reset_clip = TRUE;
  offscreen_start.bounds = &node->bounds;

  offscreen_end.force_offscreen = TRUE;
  offscreen_end.reset_clip = TRUE;
  offscreen_end.bounds = &node->bounds;

  if (!gsk_ngl_render_job_visit_node_with_offscreen (job, start_node, &offscreen_start))
    {
      gsk_ngl_render_job_visit_node (job, end_node);
      return;
    }

  g_assert (offscreen_start.texture_id);

  if (!gsk_ngl_render_job_visit_node_with_offscreen (job, end_node, &offscreen_end))
    {
      float prev_alpha = gsk_ngl_render_job_set_alpha (job, job->alpha * progress);
      gsk_ngl_render_job_visit_node (job, start_node);
      gsk_ngl_render_job_set_alpha (job, prev_alpha);
      return;
    }

  g_assert (offscreen_end.texture_id);

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, cross_fade));
  gsk_ngl_program_set_uniform_texture (job->current_program,
                                       UNIFORM_SHARED_SOURCE, 0,
                                       GL_TEXTURE_2D,
                                       GL_TEXTURE0,
                                       offscreen_start.texture_id);
  gsk_ngl_program_set_uniform_texture (job->current_program,
                                       UNIFORM_CROSS_FADE_SOURCE2, 0,
                                       GL_TEXTURE_2D,
                                       GL_TEXTURE1,
                                       offscreen_end.texture_id);
  gsk_ngl_program_set_uniform1f (job->current_program,
                                 UNIFORM_CROSS_FADE_PROGRESS, 0,
                                 progress);
  gsk_ngl_render_job_load_vertices_from_offscreen (job, &node->bounds, &offscreen_end);
  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_opacity_node (GskNglRenderJob     *job,
                                       const GskRenderNode *node)
{
  const GskRenderNode *child = gsk_opacity_node_get_child (node);
  float opacity = gsk_opacity_node_get_opacity (node);
  float new_alpha = job->alpha * opacity;

  if (!ALPHA_IS_CLEAR (new_alpha))
    {
      float prev_alpha = gsk_ngl_render_job_set_alpha (job, new_alpha);

      if (gsk_render_node_get_node_type (child) == GSK_CONTAINER_NODE)
        {
          GskNglRenderOffscreen offscreen = {0};

          offscreen.bounds = &child->bounds;
          offscreen.force_offscreen = TRUE;
          offscreen.reset_clip = TRUE;

          /* The semantics of an opacity node mandate that when, e.g., two
           * color nodes overlap, there may not be any blending between them.
           */
          if (!gsk_ngl_render_job_visit_node_with_offscreen (job, child, &offscreen))
            return;

          g_assert (offscreen.texture_id);

          gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blit));
          gsk_ngl_program_set_uniform_texture (job->current_program,
                                               UNIFORM_SHARED_SOURCE, 0,
                                               GL_TEXTURE_2D,
                                               GL_TEXTURE0,
                                               offscreen.texture_id);
          gsk_ngl_render_job_load_vertices_from_offscreen (job, &node->bounds, &offscreen);
          gsk_ngl_render_job_end_draw (job);
        }
      else
        {
          gsk_ngl_render_job_visit_node (job, child);
        }

      gsk_ngl_render_job_set_alpha (job, prev_alpha);
    }
}

static inline void
gsk_ngl_render_job_visit_text_node (GskNglRenderJob     *job,
                                    const GskRenderNode *node,
                                    const GdkRGBA       *color,
                                    gboolean             force_color)
{
  const PangoFont *font = gsk_text_node_get_font (node);
  const PangoGlyphInfo *glyphs = gsk_text_node_get_glyphs (node, NULL);
  const graphene_point_t *offset = gsk_text_node_get_offset (node);
  float text_scale = MAX (job->scale_x, job->scale_y); /* TODO: Fix for uneven scales? */
  guint num_glyphs = gsk_text_node_get_num_glyphs (node);
  float x = offset->x + job->offset_x;
  float y = offset->y + job->offset_y;
  GskNglGlyphLibrary *library = job->driver->glyphs;
  GskNglCommandBatch *batch;
  int x_position = 0;
  GskNglGlyphKey lookup;
  GskNglProgram *program;
  guint last_texture = 0;
  GskNglDrawVertex *vertices;
  guint used = 0;
  GdkRGBA c;

  if (num_glyphs == 0)
    return;

  program = CHOOSE_PROGRAM (job, coloring);

  /* If the font has color glyphs, we don't need to recolor anything.
   * We tell the shader by setting the color to vec4(-1).
   */
  if (!force_color && gsk_text_node_has_color_glyphs (node))
    c = (GdkRGBA) { -1.f, -1.f, -1.f, -1.f };
  else
    c = *color;

  lookup.font = (PangoFont *)font;
  lookup.scale = (guint) (text_scale * 1024);

  gsk_ngl_render_job_begin_draw (job, program);

  batch = gsk_ngl_command_queue_get_batch (job->command_queue);
  vertices = gsk_ngl_command_queue_add_n_vertices (job->command_queue, num_glyphs);

  /* We use one quad per character */
  for (guint i = 0; i < num_glyphs; i++)
    {
      const PangoGlyphInfo *gi = &glyphs[i];
      const GskNglGlyphValue *glyph;
      guint base = used * GSK_NGL_N_VERTICES;
      float glyph_x, glyph_y, glyph_x2, glyph_y2;
      float tx, ty, tx2, ty2;
      float cx;
      float cy;
      guint texture_id;

      if (gi->glyph == PANGO_GLYPH_EMPTY)
        continue;

      cx = (float)(x_position + gi->geometry.x_offset) / PANGO_SCALE;
      cy = (float)(gi->geometry.y_offset) / PANGO_SCALE;

      gsk_ngl_glyph_key_set_glyph_and_shift (&lookup, gi->glyph, x + cx, y + cy);

      if (!gsk_ngl_glyph_library_lookup_or_add (library, &lookup, &glyph))
        goto next;

      texture_id = GSK_NGL_TEXTURE_ATLAS_ENTRY_TEXTURE (glyph);

      if G_UNLIKELY (last_texture != texture_id)
        {
          g_assert (texture_id > 0);

          if G_LIKELY (last_texture != 0)
            {
              guint vbo_offset = batch->draw.vbo_offset + batch->draw.vbo_count;

              /* Since we have batched added our VBO vertices to avoid repeated
               * calls to the buffer, we need to manually tweak the vbo offset
               * of the new batch as otherwise it will point at the end of our
               * vbo array.
               */
              gsk_ngl_render_job_split_draw (job);
              batch = gsk_ngl_command_queue_get_batch (job->command_queue);
              batch->draw.vbo_offset = vbo_offset;
            }

          gsk_ngl_program_set_uniform_texture (program,
                                               UNIFORM_SHARED_SOURCE, 0,
                                               GL_TEXTURE_2D,
                                               GL_TEXTURE0,
                                               texture_id);
          last_texture = texture_id;
        }

      tx = glyph->entry.area.x;
      ty = glyph->entry.area.y;
      tx2 = glyph->entry.area.x2;
      ty2 = glyph->entry.area.y2;

      glyph_x = floorf (x + cx + 0.125) + glyph->ink_rect.x;
      glyph_y = floorf (y + cy + 0.125) + glyph->ink_rect.y;
      glyph_x2 = glyph_x + glyph->ink_rect.width;
      glyph_y2 = glyph_y + glyph->ink_rect.height;

      vertices[base+0].position[0] = glyph_x;
      vertices[base+0].position[1] = glyph_y;
      vertices[base+0].uv[0] = tx;
      vertices[base+0].uv[1] = ty;
      vertices[base+0].color[0] = c.red;
      vertices[base+0].color[1] = c.green;
      vertices[base+0].color[2] = c.blue;
      vertices[base+0].color[3] = c.alpha;

      vertices[base+1].position[0] = glyph_x;
      vertices[base+1].position[1] = glyph_y2;
      vertices[base+1].uv[0] = tx;
      vertices[base+1].uv[1] = ty2;
      vertices[base+1].color[0] = c.red;
      vertices[base+1].color[1] = c.green;
      vertices[base+1].color[2] = c.blue;
      vertices[base+1].color[3] = c.alpha;

      vertices[base+2].position[0] = glyph_x2;
      vertices[base+2].position[1] = glyph_y;
      vertices[base+2].uv[0] = tx2;
      vertices[base+2].uv[1] = ty;
      vertices[base+2].color[0] = c.red;
      vertices[base+2].color[1] = c.green;
      vertices[base+2].color[2] = c.blue;
      vertices[base+2].color[3] = c.alpha;

      vertices[base+3].position[0] = glyph_x2;
      vertices[base+3].position[1] = glyph_y2;
      vertices[base+3].uv[0] = tx2;
      vertices[base+3].uv[1] = ty2;
      vertices[base+3].color[0] = c.red;
      vertices[base+3].color[1] = c.green;
      vertices[base+3].color[2] = c.blue;
      vertices[base+3].color[3] = c.alpha;

      vertices[base+4].position[0] = glyph_x;
      vertices[base+4].position[1] = glyph_y2;
      vertices[base+4].uv[0] = tx;
      vertices[base+4].uv[1] = ty2;
      vertices[base+4].color[0] = c.red;
      vertices[base+4].color[1] = c.green;
      vertices[base+4].color[2] = c.blue;
      vertices[base+4].color[3] = c.alpha;

      vertices[base+5].position[0] = glyph_x2;
      vertices[base+5].position[1] = glyph_y;
      vertices[base+5].uv[0] = tx2;
      vertices[base+5].uv[1] = ty;
      vertices[base+5].color[0] = c.red;
      vertices[base+5].color[1] = c.green;
      vertices[base+5].color[2] = c.blue;
      vertices[base+5].color[3] = c.alpha;

      batch->draw.vbo_count += GSK_NGL_N_VERTICES;
      used++;

next:
      x_position += gi->geometry.width;
    }

  if (used != num_glyphs)
    gsk_ngl_command_queue_retract_n_vertices (job->command_queue, num_glyphs - used);

  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_shadow_node (GskNglRenderJob     *job,
                                      const GskRenderNode *node)
{
  const gsize n_shadows = gsk_shadow_node_get_n_shadows (node);
  const GskRenderNode *original_child = gsk_shadow_node_get_child (node);
  const GskRenderNode *shadow_child = original_child;

  /* Shadow nodes recolor every pixel of the source texture, but leave the alpha in tact.
   * If the child is a color matrix node that doesn't touch the alpha, we can throw that away. */
  if (gsk_render_node_get_node_type (shadow_child) == GSK_COLOR_MATRIX_NODE &&
      !color_matrix_modifies_alpha (shadow_child))
    shadow_child = gsk_color_matrix_node_get_child (shadow_child);

  for (guint i = 0; i < n_shadows; i++)
    {
      const GskShadow *shadow = gsk_shadow_node_get_shadow (node, i);
      const float dx = shadow->dx;
      const float dy = shadow->dy;
      GskNglRenderOffscreen offscreen = {0};
      graphene_rect_t bounds;

      if (shadow->radius == 0 &&
          gsk_render_node_get_node_type (shadow_child) == GSK_TEXT_NODE)
        {
          gsk_ngl_render_job_offset (job, dx, dy);
          gsk_ngl_render_job_visit_text_node (job, shadow_child, &shadow->color, TRUE);
          gsk_ngl_render_job_offset (job, -dx, -dy);
          continue;
        }

      if (RGBA_IS_CLEAR (&shadow->color))
        continue;

      if (node_is_invisible (shadow_child))
        continue;

      if (shadow->radius > 0)
        {
          float min_x;
          float min_y;
          float max_x;
          float max_y;

          offscreen.do_not_cache = TRUE;

          blur_node (job,
                     &offscreen,
                     shadow_child,
                     shadow->radius,
                     &min_x, &max_x,
                     &min_y, &max_y);

          bounds.origin.x = min_x - job->offset_x;
          bounds.origin.y = min_y - job->offset_y;
          bounds.size.width = max_x - min_x;
          bounds.size.height = max_y - min_y;

          offscreen.was_offscreen = TRUE;
        }
      else if (dx == 0 && dy == 0)
        {
          continue; /* Invisible anyway */
        }
      else
        {
          offscreen.bounds = &shadow_child->bounds;
          offscreen.reset_clip = TRUE;
          offscreen.do_not_cache = TRUE;

          if (!gsk_ngl_render_job_visit_node_with_offscreen (job, shadow_child, &offscreen))
            g_assert_not_reached ();

          bounds = shadow_child->bounds;
        }

      gsk_ngl_render_job_offset (job, dx, dy);
      gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, coloring));
      gsk_ngl_render_job_set_color (job, &shadow->color);
      gsk_ngl_program_set_uniform_texture (job->current_program,
                                           UNIFORM_SHARED_SOURCE, 0,
                                           GL_TEXTURE_2D,
                                           GL_TEXTURE0,
                                           offscreen.texture_id);
      gsk_ngl_render_job_load_vertices_from_offscreen (job, &bounds, &offscreen);
      gsk_ngl_render_job_end_draw (job);
      gsk_ngl_render_job_offset (job, -dx, -dy);
    }

  /* Now draw the child normally */
  gsk_ngl_render_job_visit_node (job, original_child);
}

static inline void
gsk_ngl_render_job_visit_blur_node (GskNglRenderJob     *job,
                                    const GskRenderNode *node)
{
  const GskRenderNode *child = gsk_blur_node_get_child (node);
  float blur_radius = gsk_blur_node_get_radius (node);
  GskNglRenderOffscreen offscreen = {0};
  GskTextureKey key;
  gboolean cache_texture;
  float min_x;
  float max_x;
  float min_y;
  float max_y;

  g_assert (blur_radius > 0);

  if (node_is_invisible (child))
    return;

  key.pointer = node;
  key.pointer_is_child = FALSE;
  key.scale_x = job->scale_x;
  key.scale_y = job->scale_y;
  key.filter = GL_NEAREST;

  offscreen.texture_id = gsk_ngl_driver_lookup_texture (job->driver, &key);
  cache_texture = offscreen.texture_id == 0;

  blur_node (job,
             &offscreen,
             child,
             blur_radius,
             &min_x, &max_x, &min_y, &max_y);

  g_assert (offscreen.texture_id != 0);

  if (cache_texture)
    gsk_ngl_driver_cache_texture (job->driver, &key, offscreen.texture_id);

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blit));
  gsk_ngl_program_set_uniform_texture (job->current_program,
                                       UNIFORM_SHARED_SOURCE, 0,
                                       GL_TEXTURE_2D,
                                       GL_TEXTURE0,
                                       offscreen.texture_id);
  gsk_ngl_render_job_draw_coords (job, min_x, min_y, max_x, max_y, 0, 1, 1, 0);
  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_blend_node (GskNglRenderJob     *job,
                                     const GskRenderNode *node)
{
  const GskRenderNode *top_child = gsk_blend_node_get_top_child (node);
  const GskRenderNode *bottom_child = gsk_blend_node_get_bottom_child (node);
  GskNglRenderOffscreen top_offscreen = {0};
  GskNglRenderOffscreen bottom_offscreen = {0};

  top_offscreen.bounds = &node->bounds;
  top_offscreen.force_offscreen = TRUE;
  top_offscreen.reset_clip = TRUE;

  bottom_offscreen.bounds = &node->bounds;
  bottom_offscreen.force_offscreen = TRUE;
  bottom_offscreen.reset_clip = TRUE;

  /* TODO: We create 2 textures here as big as the blend node, but both the
   * start and the end node might be a lot smaller than that. */
  if (!gsk_ngl_render_job_visit_node_with_offscreen (job, bottom_child, &bottom_offscreen))
    {
      gsk_ngl_render_job_visit_node (job, top_child);
      return;
    }

  g_assert (bottom_offscreen.was_offscreen);

  if (!gsk_ngl_render_job_visit_node_with_offscreen (job, top_child, &top_offscreen))
    {
      gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blit));
      gsk_ngl_program_set_uniform_texture (job->current_program,
                                           UNIFORM_SHARED_SOURCE, 0,
                                           GL_TEXTURE_2D,
                                           GL_TEXTURE0,
                                           bottom_offscreen.texture_id);
      gsk_ngl_render_job_load_vertices_from_offscreen (job, &node->bounds, &bottom_offscreen);
      gsk_ngl_render_job_end_draw (job);
      return;
    }

  g_assert (top_offscreen.was_offscreen);

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blend));
  gsk_ngl_program_set_uniform_texture (job->current_program,
                                       UNIFORM_SHARED_SOURCE, 0,
                                       GL_TEXTURE_2D,
                                       GL_TEXTURE0,
                                       bottom_offscreen.texture_id);
  gsk_ngl_program_set_uniform_texture (job->current_program,
                                       UNIFORM_BLEND_SOURCE2, 0,
                                       GL_TEXTURE_2D,
                                       GL_TEXTURE1,
                                       top_offscreen.texture_id);
  gsk_ngl_program_set_uniform1i (job->current_program,
                                 UNIFORM_BLEND_MODE, 0,
                                 gsk_blend_node_get_blend_mode (node));
  gsk_ngl_render_job_draw_offscreen_rect (job, &node->bounds);
  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_color_matrix_node (GskNglRenderJob     *job,
                                            const GskRenderNode *node)
{
  const GskRenderNode *child = gsk_color_matrix_node_get_child (node);
  GskNglRenderOffscreen offscreen = {0};
  float offset[4];

  if (node_is_invisible (child))
    return;

  offscreen.bounds = &node->bounds;
  offscreen.reset_clip = TRUE;

  if (!gsk_ngl_render_job_visit_node_with_offscreen (job, child, &offscreen))
    g_assert_not_reached ();

  g_assert (offscreen.texture_id > 0);

  graphene_vec4_to_float (gsk_color_matrix_node_get_color_offset (node), offset);

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, color_matrix));
  gsk_ngl_program_set_uniform_texture (job->current_program,
                                       UNIFORM_SHARED_SOURCE, 0,
                                       GL_TEXTURE_2D,
                                       GL_TEXTURE0,
                                       offscreen.texture_id);
  gsk_ngl_program_set_uniform_matrix (job->current_program,
                                      UNIFORM_COLOR_MATRIX_COLOR_MATRIX, 0,
                                      gsk_color_matrix_node_get_color_matrix (node));
  gsk_ngl_program_set_uniform4fv (job->current_program,
                                  UNIFORM_COLOR_MATRIX_COLOR_OFFSET, 0,
                                  1,
                                  offset);
  gsk_ngl_render_job_load_vertices_from_offscreen (job, &node->bounds, &offscreen);
  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_gl_shader_node_fallback (GskNglRenderJob     *job,
                                                  const GskRenderNode *node)
{
  static const GdkRGBA pink = { 255 / 255., 105 / 255., 180 / 255., 1.0 };

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, color));
  gsk_ngl_render_job_set_color (job, &pink);
  gsk_ngl_render_job_draw_rect (job, &node->bounds);
  gsk_ngl_render_job_end_draw (job);
}

static inline void
gsk_ngl_render_job_visit_gl_shader_node (GskNglRenderJob     *job,
                                         const GskRenderNode *node)
{
  GError *error = NULL;
  GskGLShader *shader;
  GskNglProgram *program;
  int n_children;

  shader = gsk_gl_shader_node_get_shader (node);
  program = gsk_ngl_driver_lookup_shader (job->driver, shader, &error);
  n_children = gsk_gl_shader_node_get_n_children (node);

  if G_UNLIKELY (program == NULL)
    {
      if (g_object_get_data (G_OBJECT (shader), "gsk-did-warn") == NULL)
        {
          g_object_set_data (G_OBJECT (shader), "gsk-did-warn", GUINT_TO_POINTER (1));
          g_warning ("Failed to compile gl shader: %s", error->message);
        }
      gsk_ngl_render_job_visit_gl_shader_node_fallback (job, node);
      g_clear_error (&error);
    }
  else
    {
      GskNglRenderOffscreen offscreens[4] = {{0}};
      const GskGLUniform *uniforms;
      const guint8 *base;
      GBytes *args;
      int n_uniforms;

      g_assert (n_children < G_N_ELEMENTS (offscreens));

      for (guint i = 0; i < n_children; i++)
        {
          const GskRenderNode *child = gsk_gl_shader_node_get_child (node, i);

          offscreens[i].bounds = &node->bounds;
          offscreens[i].force_offscreen = TRUE;
          offscreens[i].reset_clip = TRUE;

          if (!gsk_ngl_render_job_visit_node_with_offscreen (job, child, &offscreens[i]))
            return;
        }

      args = gsk_gl_shader_node_get_args (node);
      base = g_bytes_get_data (args, NULL);
      uniforms = gsk_gl_shader_get_uniforms (shader, &n_uniforms);

      gsk_ngl_render_job_begin_draw (job, program);
      for (guint i = 0; i < n_children; i++)
        gsk_ngl_program_set_uniform_texture (program,
                                             UNIFORM_CUSTOM_TEXTURE1 + i, 0,
                                             GL_TEXTURE_2D,
                                             GL_TEXTURE0 + i,
                                             offscreens[i].texture_id);
      gsk_ngl_program_set_uniform2f (program,
                                     UNIFORM_CUSTOM_SIZE, 0,
                                     node->bounds.size.width,
                                     node->bounds.size.height);
      for (guint i = 0; i < n_uniforms; i++)
        {
          const GskGLUniform *u = &uniforms[i];
          const guint8 *data = base + u->offset;

          /* Ignore unused uniforms */
          if (program->args_locations[i] == -1)
            continue;

          switch (u->type)
            {
            default:
            case GSK_GL_UNIFORM_TYPE_NONE:
              break;
            case GSK_GL_UNIFORM_TYPE_FLOAT:
              gsk_ngl_uniform_state_set1fv (job->command_queue->uniforms,
                                            program->program_info,
                                            program->args_locations[i],
                                            0, 1, (const float *)data);
              break;
            case GSK_GL_UNIFORM_TYPE_INT:
              gsk_ngl_uniform_state_set1i (job->command_queue->uniforms,
                                           program->program_info,
                                           program->args_locations[i],
                                           0, *(const gint32 *)data);
              break;
            case GSK_GL_UNIFORM_TYPE_UINT:
            case GSK_GL_UNIFORM_TYPE_BOOL:
              gsk_ngl_uniform_state_set1ui (job->command_queue->uniforms,
                                            program->program_info,
                                            program->args_locations[i],
                                            0, *(const guint32 *)data);
              break;
            case GSK_GL_UNIFORM_TYPE_VEC2:
              gsk_ngl_uniform_state_set2fv (job->command_queue->uniforms,
                                            program->program_info,
                                            program->args_locations[i],
                                            0, 1, (const float *)data);
              break;
            case GSK_GL_UNIFORM_TYPE_VEC3:
              gsk_ngl_uniform_state_set3fv (job->command_queue->uniforms,
                                            program->program_info,
                                            program->args_locations[i],
                                            0, 1, (const float *)data);
              break;
            case GSK_GL_UNIFORM_TYPE_VEC4:
              gsk_ngl_uniform_state_set4fv (job->command_queue->uniforms,
                                            program->program_info,
                                            program->args_locations[i],
                                            0, 1, (const float *)data);
              break;
            }
        }
      gsk_ngl_render_job_draw_offscreen_rect (job, &node->bounds);
      gsk_ngl_render_job_end_draw (job);
    }
}

static void
gsk_ngl_render_job_upload_texture (GskNglRenderJob       *job,
                                   GdkTexture            *texture,
                                   GskNglRenderOffscreen *offscreen)
{
  if (gsk_ngl_texture_library_can_cache (GSK_NGL_TEXTURE_LIBRARY (job->driver->icons),
                                        texture->width,
                                        texture->height) &&
      !GDK_IS_GL_TEXTURE (texture))
    {
      const GskNglIconData *icon_data;

      gsk_ngl_icon_library_lookup_or_add (job->driver->icons, texture, &icon_data);
      offscreen->texture_id = GSK_NGL_TEXTURE_ATLAS_ENTRY_TEXTURE (icon_data);
      memcpy (&offscreen->area, &icon_data->entry.area, sizeof offscreen->area);
    }
  else
    {
      offscreen->texture_id = gsk_ngl_driver_load_texture (job->driver, texture, GL_LINEAR, GL_LINEAR);
      init_full_texture_region (offscreen);
    }
}

static inline void
gsk_ngl_render_job_visit_texture_node (GskNglRenderJob     *job,
                                       const GskRenderNode *node)
{
  GdkTexture *texture = gsk_texture_node_get_texture (node);
  int max_texture_size = job->command_queue->max_texture_size;

  if G_LIKELY (texture->width <= max_texture_size &&
               texture->height <= max_texture_size)
    {
      GskNglRenderOffscreen offscreen = {0};

      gsk_ngl_render_job_upload_texture (job, texture, &offscreen);

      g_assert (offscreen.texture_id);
      g_assert (offscreen.was_offscreen == FALSE);

      gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blit));
      gsk_ngl_program_set_uniform_texture (job->current_program,
                                           UNIFORM_SHARED_SOURCE, 0,
                                           GL_TEXTURE_2D,
                                           GL_TEXTURE0,
                                           offscreen.texture_id);
      gsk_ngl_render_job_load_vertices_from_offscreen (job, &node->bounds, &offscreen);
      gsk_ngl_render_job_end_draw (job);
    }
  else
    {
      float min_x = job->offset_x + node->bounds.origin.x;
      float min_y = job->offset_y + node->bounds.origin.y;
      float max_x = min_x + node->bounds.size.width;
      float max_y = min_y + node->bounds.size.height;
      float scale_x = (max_x - min_x) / texture->width;
      float scale_y = (max_y - min_y) / texture->height;
      GskNglTextureSlice *slices = NULL;
      guint n_slices = 0;

      gsk_ngl_driver_slice_texture (job->driver, texture, &slices, &n_slices);

      g_assert (slices != NULL);
      g_assert (n_slices > 0);

      gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blit));

      for (guint i = 0; i < n_slices; i ++)
        {
          const GskNglTextureSlice *slice = &slices[i];
          float x1, x2, y1, y2;

          x1 = min_x + (scale_x * slice->rect.x);
          x2 = x1 + (slice->rect.width * scale_x);
          y1 = min_y + (scale_y * slice->rect.y);
          y2 = y1 + (slice->rect.height * scale_y);

          if (i > 0)
            gsk_ngl_render_job_split_draw (job);
          gsk_ngl_program_set_uniform_texture (job->current_program,
                                               UNIFORM_SHARED_SOURCE, 0,
                                               GL_TEXTURE_2D,
                                               GL_TEXTURE0,
                                               slice->texture_id);

          gsk_ngl_render_job_draw_coords (job, x1, y1, x2, y2, 0, 0, 1, 1);
        }

      gsk_ngl_render_job_end_draw (job);
    }
}

static inline void
gsk_ngl_render_job_visit_repeat_node (GskNglRenderJob     *job,
                                      const GskRenderNode *node)
{
  const GskRenderNode *child = gsk_repeat_node_get_child (node);
  const graphene_rect_t *child_bounds = gsk_repeat_node_get_child_bounds (node);
  GskNglRenderOffscreen offscreen = {0};

  if (node_is_invisible (child))
    return;

  if (!graphene_rect_equal (child_bounds, &child->bounds))
    {
      /* TODO: implement these repeat nodes. */
      gsk_ngl_render_job_visit_as_fallback (job, node);
      return;
    }

  /* If the size of the repeat node is smaller than the size of the
   * child node, we don't repeat at all and can just draw that part
   * of the child texture... */
  if (rect_contains_rect (child_bounds, &node->bounds))
    {
      gsk_ngl_render_job_visit_clipped_child (job, child, &node->bounds);
      return;
    }

  offscreen.bounds = &child->bounds;
  offscreen.reset_clip = TRUE;

  if (!gsk_ngl_render_job_visit_node_with_offscreen (job, child, &offscreen))
    g_assert_not_reached ();

  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, repeat));
  gsk_ngl_program_set_uniform_texture (job->current_program,
                                       UNIFORM_SHARED_SOURCE, 0,
                                       GL_TEXTURE_2D,
                                       GL_TEXTURE0,
                                       offscreen.texture_id);
  gsk_ngl_program_set_uniform4f (job->current_program,
                                 UNIFORM_REPEAT_CHILD_BOUNDS, 0,
                                 (node->bounds.origin.x - child_bounds->origin.x) / child_bounds->size.width,
                                 (node->bounds.origin.y - child_bounds->origin.y) / child_bounds->size.height,
                                 node->bounds.size.width / child_bounds->size.width,
                                 node->bounds.size.height / child_bounds->size.height);
  gsk_ngl_program_set_uniform4f (job->current_program,
                                 UNIFORM_REPEAT_TEXTURE_RECT, 0,
                                 offscreen.area.x,
                                 offscreen.was_offscreen ? offscreen.area.y2 : offscreen.area.y,
                                 offscreen.area.x2,
                                 offscreen.was_offscreen ? offscreen.area.y : offscreen.area.y2);
  gsk_ngl_render_job_load_vertices_from_offscreen (job, &node->bounds, &offscreen);
  gsk_ngl_render_job_end_draw (job);
}

static void
gsk_ngl_render_job_visit_node (GskNglRenderJob     *job,
                               const GskRenderNode *node)
{
  gboolean has_clip;

  g_assert (job != NULL);
  g_assert (node != NULL);
  g_assert (GSK_IS_NGL_DRIVER (job->driver));
  g_assert (GSK_IS_NGL_COMMAND_QUEUE (job->command_queue));

  if (node_is_invisible (node))
    return;

  if (!gsk_ngl_render_job_update_clip (job, node, &has_clip))
    return;

  switch (gsk_render_node_get_node_type (node))
    {
    case GSK_BLEND_NODE:
      gsk_ngl_render_job_visit_blend_node (job, node);
    break;

    case GSK_BLUR_NODE:
      if (gsk_blur_node_get_radius (node) > 0)
        gsk_ngl_render_job_visit_blur_node (job, node);
      else
        gsk_ngl_render_job_visit_node (job, gsk_blur_node_get_child (node));
    break;

    case GSK_BORDER_NODE:
      if (gsk_border_node_get_uniform_color (node) &&
          gsk_rounded_rect_is_rectilinear (gsk_border_node_get_outline (node)))
        gsk_ngl_render_job_visit_rect_border_node (job, node);
      else if (gsk_border_node_get_uniform (node))
        gsk_ngl_render_job_visit_uniform_border_node (job, node);
      else
        gsk_ngl_render_job_visit_border_node (job, node);
    break;

    case GSK_CLIP_NODE:
      gsk_ngl_render_job_visit_clip_node (job, node);
    break;

    case GSK_COLOR_NODE:
      gsk_ngl_render_job_visit_color_node (job, node);
    break;

    case GSK_COLOR_MATRIX_NODE:
      gsk_ngl_render_job_visit_color_matrix_node (job, node);
    break;

    case GSK_CONIC_GRADIENT_NODE:
      if (gsk_conic_gradient_node_get_n_color_stops (node) < MAX_GRADIENT_STOPS)
        gsk_ngl_render_job_visit_conic_gradient_node (job, node);
      else
        gsk_ngl_render_job_visit_as_fallback (job, node);
    break;

    case GSK_CONTAINER_NODE:
      {
        guint n_children = gsk_container_node_get_n_children (node);

        for (guint i = 0; i < n_children; i++)
          {
            const GskRenderNode *child = gsk_container_node_get_child (node, i);
            gsk_ngl_render_job_visit_node (job, child);
          }
      }
    break;

    case GSK_CROSS_FADE_NODE:
      {
        const GskRenderNode *start_node = gsk_cross_fade_node_get_start_child (node);
        const GskRenderNode *end_node = gsk_cross_fade_node_get_end_child (node);
        float progress = gsk_cross_fade_node_get_progress (node);

        if (progress <= 0.0f)
          gsk_ngl_render_job_visit_node (job, gsk_cross_fade_node_get_start_child (node));
        else if (progress >= 1.0f || equal_texture_nodes (start_node, end_node))
          gsk_ngl_render_job_visit_node (job, gsk_cross_fade_node_get_end_child (node));
        else
          gsk_ngl_render_job_visit_cross_fade_node (job, node);
      }
    break;

    case GSK_DEBUG_NODE:
      /* Debug nodes are ignored because draws get reordered anyway */
      gsk_ngl_render_job_visit_node (job, gsk_debug_node_get_child (node));
    break;

    case GSK_GL_SHADER_NODE:
      gsk_ngl_render_job_visit_gl_shader_node (job, node);
    break;

    case GSK_INSET_SHADOW_NODE:
      if (gsk_inset_shadow_node_get_blur_radius (node) > 0)
        gsk_ngl_render_job_visit_blurred_inset_shadow_node (job, node);
      else
        gsk_ngl_render_job_visit_unblurred_inset_shadow_node (job, node);
    break;

    case GSK_LINEAR_GRADIENT_NODE:
    case GSK_REPEATING_LINEAR_GRADIENT_NODE:
      if (gsk_linear_gradient_node_get_n_color_stops (node) < MAX_GRADIENT_STOPS)
        gsk_ngl_render_job_visit_linear_gradient_node (job, node);
      else
        gsk_ngl_render_job_visit_as_fallback (job, node);
    break;

    case GSK_OPACITY_NODE:
      gsk_ngl_render_job_visit_opacity_node (job, node);
    break;

    case GSK_OUTSET_SHADOW_NODE:
      if (gsk_outset_shadow_node_get_blur_radius (node) > 0)
        gsk_ngl_render_job_visit_blurred_outset_shadow_node (job, node);
      else
        gsk_ngl_render_job_visit_unblurred_outset_shadow_node (job, node);
    break;

    case GSK_RADIAL_GRADIENT_NODE:
    case GSK_REPEATING_RADIAL_GRADIENT_NODE:
      if (gsk_radial_gradient_node_get_n_color_stops (node) < MAX_GRADIENT_STOPS)
        gsk_ngl_render_job_visit_radial_gradient_node (job, node);
      else
        gsk_ngl_render_job_visit_as_fallback (job, node);
    break;

    case GSK_REPEAT_NODE:
      gsk_ngl_render_job_visit_repeat_node (job, node);
    break;

    case GSK_ROUNDED_CLIP_NODE:
      gsk_ngl_render_job_visit_rounded_clip_node (job, node);
    break;

    case GSK_SHADOW_NODE:
      gsk_ngl_render_job_visit_shadow_node (job, node);
    break;

    case GSK_TEXT_NODE:
      gsk_ngl_render_job_visit_text_node (job,
                                          node,
                                          gsk_text_node_get_color (node),
                                          FALSE);
    break;

    case GSK_TEXTURE_NODE:
      gsk_ngl_render_job_visit_texture_node (job, node);
    break;

    case GSK_TRANSFORM_NODE:
      gsk_ngl_render_job_visit_transform_node (job, node);
    break;

    case GSK_CAIRO_NODE:
      gsk_ngl_render_job_visit_as_fallback (job, node);
    break;

    case GSK_NOT_A_RENDER_NODE:
    default:
      g_assert_not_reached ();
    break;
    }

  if (has_clip)
    gsk_ngl_render_job_pop_clip (job);
}

static gboolean
gsk_ngl_render_job_visit_node_with_offscreen (GskNglRenderJob       *job,
                                              const GskRenderNode   *node,
                                              GskNglRenderOffscreen *offscreen)
{
  GskTextureKey key;
  guint cached_id;
  int filter;

  g_assert (job != NULL);
  g_assert (node != NULL);
  g_assert (offscreen != NULL);
  g_assert (offscreen->texture_id == 0);
  g_assert (offscreen->bounds != NULL);

  if (node_is_invisible (node))
    {
      /* Just to be safe. */
      offscreen->texture_id = 0;
      init_full_texture_region (offscreen);
      offscreen->was_offscreen = FALSE;
      return FALSE;
    }

  if (gsk_render_node_get_node_type (node) == GSK_TEXTURE_NODE &&
      offscreen->force_offscreen == FALSE)
    {
      GdkTexture *texture = gsk_texture_node_get_texture (node);
      gsk_ngl_render_job_upload_texture (job, texture, offscreen);
      g_assert (offscreen->was_offscreen == FALSE);
      return TRUE;
    }

  filter = offscreen->linear_filter ? GL_LINEAR : GL_NEAREST;

  /* Check if we've already cached the drawn texture. */
  key.pointer = node;
  key.pointer_is_child = TRUE; /* Don't conflict with the child using the cache too */
  key.parent_rect = *offscreen->bounds;
  key.scale_x = job->scale_x;
  key.scale_y = job->scale_y;
  key.filter = filter;

  cached_id = gsk_ngl_driver_lookup_texture (job->driver, &key);

  if (cached_id != 0)
    {
      offscreen->texture_id = cached_id;
      init_full_texture_region (offscreen);
      /* We didn't render it offscreen, but hand out an offscreen texture id */
      offscreen->was_offscreen = TRUE;
      return TRUE;
    }

  float scaled_width;
  float scaled_height;
  float scale_x = job->scale_x;
  float scale_y = job->scale_y;

  g_assert (job->command_queue->max_texture_size > 0);

  /* Tweak the scale factor so that the required texture doesn't
   * exceed the max texture limit. This will render with a lower
   * resolution, but this is better than clipping.
   */
  {
    int max_texture_size = job->command_queue->max_texture_size;

    scaled_width = ceilf (offscreen->bounds->size.width * scale_x);
    if (scaled_width > max_texture_size)
      {
        scale_x *= (float)max_texture_size / scaled_width;
        scaled_width = max_texture_size;
      }

    scaled_height = ceilf (offscreen->bounds->size.height * scale_y);
    if (scaled_height > max_texture_size)
      {
        scale_y *= (float)max_texture_size / scaled_height;
        scaled_height = max_texture_size;
      }
  }

  GskNglRenderTarget *render_target;
  graphene_matrix_t prev_projection;
  graphene_rect_t prev_viewport;
  graphene_rect_t viewport;
  float offset_x = job->offset_x;
  float offset_y = job->offset_y;
  float prev_alpha;
  guint prev_fbo;

  if (!gsk_ngl_driver_create_render_target (job->driver,
                                             scaled_width, scaled_height,
                                             filter, filter,
                                             &render_target))
    g_assert_not_reached ();

  if (gdk_gl_context_has_debug (job->command_queue->context))
    {
      gdk_gl_context_label_object_printf (job->command_queue->context,
                                          GL_TEXTURE,
                                          render_target->texture_id,
                                          "Offscreen<%s> %d",
                                          g_type_name_from_instance ((GTypeInstance *) node),
                                          render_target->texture_id);
      gdk_gl_context_label_object_printf (job->command_queue->context,
                                          GL_FRAMEBUFFER,
                                          render_target->framebuffer_id,
                                          "Offscreen<%s> FB %d",
                                          g_type_name_from_instance ((GTypeInstance *) node),
                                          render_target->framebuffer_id);
    }

  gsk_ngl_render_job_transform_bounds (job, offscreen->bounds, &viewport);
  /* Code above will scale the size with the scale we use in the render ops,
   * but for the viewport size, we need our own size limited by the texture size */
  viewport.size.width = scaled_width;
  viewport.size.height = scaled_height;

  gsk_ngl_render_job_set_viewport (job, &viewport, &prev_viewport);
  gsk_ngl_render_job_set_projection_from_rect (job, &job->viewport, &prev_projection);
  gsk_ngl_render_job_set_modelview (job, gsk_transform_scale (NULL, scale_x, scale_y));
  prev_alpha = gsk_ngl_render_job_set_alpha (job, 1.0f);
  job->offset_x = offset_x;
  job->offset_y = offset_y;

  prev_fbo = gsk_ngl_command_queue_bind_framebuffer (job->command_queue, render_target->framebuffer_id);
  gsk_ngl_command_queue_clear (job->command_queue, 0, &job->viewport);

  if (offscreen->reset_clip)
    gsk_ngl_render_job_push_clip (job, &GSK_ROUNDED_RECT_INIT_FROM_RECT (job->viewport));

  gsk_ngl_render_job_visit_node (job, node);

  if (offscreen->reset_clip)
    gsk_ngl_render_job_pop_clip (job);

  gsk_ngl_render_job_pop_modelview (job);
  gsk_ngl_render_job_set_viewport (job, &prev_viewport, NULL);
  gsk_ngl_render_job_set_projection (job, &prev_projection);
  gsk_ngl_render_job_set_alpha (job, prev_alpha);
  gsk_ngl_command_queue_bind_framebuffer (job->command_queue, prev_fbo);

  job->offset_x = offset_x;
  job->offset_y = offset_y;

  offscreen->was_offscreen = TRUE;
  offscreen->texture_id = gsk_ngl_driver_release_render_target (job->driver,
                                                                render_target,
                                                                FALSE);

  init_full_texture_region (offscreen);

  if (!offscreen->do_not_cache)
    gsk_ngl_driver_cache_texture (job->driver, &key, offscreen->texture_id);

  return TRUE;
}

void
gsk_ngl_render_job_render_flipped (GskNglRenderJob *job,
                                   GskRenderNode   *root)
{
  graphene_matrix_t proj;
  guint framebuffer_id;
  guint texture_id;
  guint surface_height;

  g_return_if_fail (job != NULL);
  g_return_if_fail (root != NULL);
  g_return_if_fail (GSK_IS_NGL_DRIVER (job->driver));

  surface_height = job->viewport.size.height;

  graphene_matrix_init_ortho (&proj,
                              job->viewport.origin.x,
                              job->viewport.origin.x + job->viewport.size.width,
                              job->viewport.origin.y,
                              job->viewport.origin.y + job->viewport.size.height,
                              ORTHO_NEAR_PLANE,
                              ORTHO_FAR_PLANE);
  graphene_matrix_scale (&proj, 1, -1, 1);

  if (!gsk_ngl_command_queue_create_render_target (job->command_queue,
                                                  MAX (1, job->viewport.size.width),
                                                  MAX (1, job->viewport.size.height),
                                                  GL_NEAREST, GL_NEAREST,
                                                  &framebuffer_id, &texture_id))
    return;

  /* Setup drawing to our offscreen texture/framebuffer which is flipped */
  gsk_ngl_command_queue_bind_framebuffer (job->command_queue, framebuffer_id);
  gsk_ngl_command_queue_clear (job->command_queue, 0, &job->viewport);

  /* Visit all nodes creating batches */
  gdk_gl_context_push_debug_group (job->command_queue->context, "Building command queue");
  gsk_ngl_render_job_visit_node (job, root);
  gdk_gl_context_pop_debug_group (job->command_queue->context);

  /* Now draw to our real destination, but flipped */
  gsk_ngl_render_job_set_alpha (job, 1.0f);
  gsk_ngl_command_queue_bind_framebuffer (job->command_queue, job->framebuffer);
  gsk_ngl_command_queue_clear (job->command_queue, 0, &job->viewport);
  gsk_ngl_render_job_begin_draw (job, CHOOSE_PROGRAM (job, blit));
  gsk_ngl_program_set_uniform_texture (job->current_program,
                                       UNIFORM_SHARED_SOURCE, 0,
                                       GL_TEXTURE_2D,
                                       GL_TEXTURE0,
                                       texture_id);
  gsk_ngl_render_job_draw_rect (job, &job->viewport);
  gsk_ngl_render_job_end_draw (job);

  gdk_gl_context_push_debug_group (job->command_queue->context, "Executing command queue");
  gsk_ngl_command_queue_execute (job->command_queue, surface_height, 1, NULL);
  gdk_gl_context_pop_debug_group (job->command_queue->context);

  glDeleteFramebuffers (1, &framebuffer_id);
  glDeleteTextures (1, &texture_id);
}

void
gsk_ngl_render_job_render (GskNglRenderJob *job,
                           GskRenderNode   *root)
{
  G_GNUC_UNUSED gint64 start_time;
  guint scale_factor;
  guint surface_height;

  g_return_if_fail (job != NULL);
  g_return_if_fail (root != NULL);
  g_return_if_fail (GSK_IS_NGL_DRIVER (job->driver));

  scale_factor = MAX (job->scale_x, job->scale_y);
  surface_height = job->viewport.size.height;

  gsk_ngl_command_queue_make_current (job->command_queue);

  /* Build the command queue using the shared GL context for all renderers
   * on the same display.
   */
  start_time = GDK_PROFILER_CURRENT_TIME;
  gdk_gl_context_push_debug_group (job->command_queue->context, "Building command queue");
  gsk_ngl_command_queue_bind_framebuffer (job->command_queue, job->framebuffer);
  gsk_ngl_command_queue_clear (job->command_queue, 0, &job->viewport);
  gsk_ngl_render_job_visit_node (job, root);
  gdk_gl_context_pop_debug_group (job->command_queue->context);
  gdk_profiler_add_mark (start_time, GDK_PROFILER_CURRENT_TIME-start_time, "Build GL command queue", "");

#if 0
  /* At this point the atlases have uploaded content while we processed
   * nodes but have not necessarily been used by the commands in the queue.
   */
  gsk_ngl_driver_save_atlases_to_png (job->driver, NULL);
#endif

  /* But now for executing the command queue, we want to use the context
   * that was provided to us when creating the render job as framebuffer 0
   * is bound to that context.
   */
  start_time = GDK_PROFILER_CURRENT_TIME;
  gsk_ngl_command_queue_make_current (job->command_queue);
  gdk_gl_context_push_debug_group (job->command_queue->context, "Executing command queue");
  gsk_ngl_command_queue_execute (job->command_queue, surface_height, scale_factor, job->region);
  gdk_gl_context_pop_debug_group (job->command_queue->context);
  gdk_profiler_add_mark (start_time, GDK_PROFILER_CURRENT_TIME-start_time, "Execute GL command queue", "");
}

void
gsk_ngl_render_job_set_debug_fallback (GskNglRenderJob *job,
                                       gboolean         debug_fallback)
{
  g_return_if_fail (job != NULL);

  job->debug_fallback = !!debug_fallback;
}

GskNglRenderJob *
gsk_ngl_render_job_new (GskNglDriver          *driver,
                        const graphene_rect_t *viewport,
                        float                  scale_factor,
                        const cairo_region_t  *region,
                        guint                  framebuffer)
{
  const graphene_rect_t *clip_rect = viewport;
  graphene_rect_t transformed_extents;
  GskNglRenderJob *job;

  g_return_val_if_fail (GSK_IS_NGL_DRIVER (driver), NULL);
  g_return_val_if_fail (viewport != NULL, NULL);
  g_return_val_if_fail (scale_factor > 0, NULL);

  job = g_slice_new0 (GskNglRenderJob);
  job->driver = g_object_ref (driver);
  job->command_queue = job->driver->command_queue;
  job->clip = g_array_sized_new (FALSE, FALSE, sizeof (GskNglRenderClip), 16);
  job->modelview = g_array_sized_new (FALSE, FALSE, sizeof (GskNglRenderModelview), 16);
  job->framebuffer = framebuffer;
  job->offset_x = 0;
  job->offset_y = 0;
  job->scale_x = scale_factor;
  job->scale_y = scale_factor;
  job->viewport = *viewport;

  gsk_ngl_render_job_set_alpha (job, 1.0);
  gsk_ngl_render_job_set_color (job, &(GdkRGBA){ 0.f, 0.f, 0.f, 0.f });
  gsk_ngl_render_job_set_projection_from_rect (job, viewport, NULL);
  gsk_ngl_render_job_set_modelview (job, gsk_transform_scale (NULL, scale_factor, scale_factor));

  /* Setup our initial clip. If region is NULL then we are drawing the
   * whole viewport. Otherwise, we need to convert the region to a
   * bounding box and clip based on that.
   */

  if (region != NULL)
    {
      cairo_rectangle_int_t extents;

      cairo_region_get_extents (region, &extents);
      gsk_ngl_render_job_transform_bounds (job,
                                           &GRAPHENE_RECT_INIT (extents.x,
                                                                extents.y,
                                                                extents.width,
                                                                extents.height),
                                           &transformed_extents);
      clip_rect = &transformed_extents;
      job->region = cairo_region_create_rectangle (&extents);
    }

  gsk_ngl_render_job_push_clip (job,
                                &GSK_ROUNDED_RECT_INIT (clip_rect->origin.x,
                                                        clip_rect->origin.y,
                                                        clip_rect->size.width,
                                                        clip_rect->size.height));

  return job;
}

void
gsk_ngl_render_job_free (GskNglRenderJob *job)
{
  job->current_modelview = NULL;
  job->current_clip = NULL;

  while (job->modelview->len > 0)
    {
      GskNglRenderModelview *modelview = &g_array_index (job->modelview, GskNglRenderModelview, job->modelview->len-1);
      g_clear_pointer (&modelview->transform, gsk_transform_unref);
      job->modelview->len--;
    }

  g_clear_object (&job->driver);
  g_clear_pointer (&job->region, cairo_region_destroy);
  g_clear_pointer (&job->modelview, g_array_unref);
  g_clear_pointer (&job->clip, g_array_unref);
  g_slice_free (GskNglRenderJob, job);
}

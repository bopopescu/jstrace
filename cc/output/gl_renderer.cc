// Copyright 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/output/gl_renderer.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/trace_event/trace_event.h"
#include "build/build_config.h"
#include "cc/base/container_util.h"
#include "cc/base/math_util.h"
#include "cc/output/compositor_frame.h"
#include "cc/output/compositor_frame_metadata.h"
#include "cc/output/context_provider.h"
#include "cc/output/copy_output_request.h"
#include "cc/output/dynamic_geometry_binding.h"
#include "cc/output/gl_frame_data.h"
#include "cc/output/layer_quad.h"
#include "cc/output/output_surface.h"
#include "cc/output/render_surface_filters.h"
#include "cc/output/static_geometry_binding.h"
#include "cc/output/texture_mailbox_deleter.h"
#include "cc/quads/draw_polygon.h"
#include "cc/quads/picture_draw_quad.h"
#include "cc/quads/render_pass.h"
#include "cc/quads/stream_video_draw_quad.h"
#include "cc/quads/texture_draw_quad.h"
#include "cc/raster/scoped_gpu_raster.h"
#include "cc/resources/resource_pool.h"
#include "cc/resources/scoped_resource.h"
#include "gpu/GLES2/gl2extchromium.h"
#include "gpu/command_buffer/client/context_support.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "gpu/command_buffer/common/gpu_memory_allocation.h"
#include "skia/ext/texture_handle.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkColorFilter.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/gpu/GrContext.h"
#include "third_party/skia/include/gpu/gl/GrGLInterface.h"
#include "third_party/skia/include/gpu/gl/GrGLTypes.h"
#include "ui/gfx/geometry/quad_f.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/skia_util.h"

using gpu::gles2::GLES2Interface;

namespace cc {
namespace {

Float4 UVTransform(const TextureDrawQuad* quad) {
  gfx::PointF uv0 = quad->uv_top_left;
  gfx::PointF uv1 = quad->uv_bottom_right;
  Float4 xform = {{uv0.x(), uv0.y(), uv1.x() - uv0.x(), uv1.y() - uv0.y()}};
  if (quad->y_flipped) {
    xform.data[1] = 1.0f - xform.data[1];
    xform.data[3] = -xform.data[3];
  }
  return xform;
}

Float4 PremultipliedColor(SkColor color) {
  const float factor = 1.0f / 255.0f;
  const float alpha = SkColorGetA(color) * factor;

  Float4 result = {
      {SkColorGetR(color) * factor * alpha, SkColorGetG(color) * factor * alpha,
       SkColorGetB(color) * factor * alpha, alpha}};
  return result;
}

SamplerType SamplerTypeFromTextureTarget(GLenum target) {
  switch (target) {
    case GL_TEXTURE_2D:
      return SAMPLER_TYPE_2D;
    case GL_TEXTURE_RECTANGLE_ARB:
      return SAMPLER_TYPE_2D_RECT;
    case GL_TEXTURE_EXTERNAL_OES:
      return SAMPLER_TYPE_EXTERNAL_OES;
    default:
      NOTREACHED();
      return SAMPLER_TYPE_2D;
  }
}

BlendMode BlendModeFromSkXfermode(SkXfermode::Mode mode) {
  switch (mode) {
    case SkXfermode::kSrcOver_Mode:
      return BLEND_MODE_NORMAL;
    case SkXfermode::kScreen_Mode:
      return BLEND_MODE_SCREEN;
    case SkXfermode::kOverlay_Mode:
      return BLEND_MODE_OVERLAY;
    case SkXfermode::kDarken_Mode:
      return BLEND_MODE_DARKEN;
    case SkXfermode::kLighten_Mode:
      return BLEND_MODE_LIGHTEN;
    case SkXfermode::kColorDodge_Mode:
      return BLEND_MODE_COLOR_DODGE;
    case SkXfermode::kColorBurn_Mode:
      return BLEND_MODE_COLOR_BURN;
    case SkXfermode::kHardLight_Mode:
      return BLEND_MODE_HARD_LIGHT;
    case SkXfermode::kSoftLight_Mode:
      return BLEND_MODE_SOFT_LIGHT;
    case SkXfermode::kDifference_Mode:
      return BLEND_MODE_DIFFERENCE;
    case SkXfermode::kExclusion_Mode:
      return BLEND_MODE_EXCLUSION;
    case SkXfermode::kMultiply_Mode:
      return BLEND_MODE_MULTIPLY;
    case SkXfermode::kHue_Mode:
      return BLEND_MODE_HUE;
    case SkXfermode::kSaturation_Mode:
      return BLEND_MODE_SATURATION;
    case SkXfermode::kColor_Mode:
      return BLEND_MODE_COLOR;
    case SkXfermode::kLuminosity_Mode:
      return BLEND_MODE_LUMINOSITY;
    default:
      NOTREACHED();
      return BLEND_MODE_NONE;
  }
}

// Smallest unit that impact anti-aliasing output. We use this to
// determine when anti-aliasing is unnecessary.
const float kAntiAliasingEpsilon = 1.0f / 1024.0f;

// Block or crash if the number of pending sync queries reach this high as
// something is seriously wrong on the service side if this happens.
const size_t kMaxPendingSyncQueries = 16;
}  // anonymous namespace

// Parameters needed to draw a RenderPassDrawQuad.
struct DrawRenderPassDrawQuadParams {
  DrawRenderPassDrawQuadParams() {}
  ~DrawRenderPassDrawQuadParams() {}

  // Required Inputs.
  const RenderPassDrawQuad* quad = nullptr;
  const Resource* contents_texture = nullptr;
  const gfx::QuadF* clip_region = nullptr;
  bool flip_texture = false;

  gfx::Transform window_matrix;
  gfx::Transform projection_matrix;

  // |frame| is needed for background effects.
  DirectRenderer::DrawingFrame* frame = nullptr;

  // Whether the texture to be sampled from needs to be flipped.
  bool source_needs_flip = false;

  float edge[24];
  SkScalar color_matrix[20];

  // Blending refers to modifications to the backdrop.
  bool use_shaders_for_blending = false;
  ShaderLocations locations;

  bool use_aa = false;

  // Some filters affect pixels outside the original contents bounds. This
  // requires translation of the source when texturing, as well as a change in
  // the bounds of the destination.
  gfx::Point src_offset;
  gfx::RectF dst_rect;

  // A Skia image that should be sampled from instead of the original
  // contents.
  sk_sp<SkImage> filter_image;

  // The original contents, bound for sampling.
  std::unique_ptr<ResourceProvider::ScopedSamplerGL> contents_resource_lock;

  // A mask to be applied when drawing the RPDQ.
  std::unique_ptr<ResourceProvider::ScopedSamplerGL> mask_resource_lock;

  // Original background texture.
  std::unique_ptr<ScopedResource> background_texture;
  std::unique_ptr<ResourceProvider::ScopedSamplerGL>
      shader_background_sampler_lock;

  // Backdrop bounding box.
  gfx::Rect background_rect;

  // Filtered background texture.
  sk_sp<SkImage> background_image;
  GLuint background_image_id = 0;

  // Whether the original background texture is needed for the mask.
  bool mask_for_background = false;

  // Whether a color matrix needs to be applied by the shaders when drawing
  // the RPDQ.
  bool use_color_matrix = false;

  gfx::QuadF surface_quad;
  gfx::Transform contents_device_transform;
};

static GLint GetActiveTextureUnit(GLES2Interface* gl) {
  GLint active_unit = 0;
  gl->GetIntegerv(GL_ACTIVE_TEXTURE, &active_unit);
  return active_unit;
}

class GLRenderer::ScopedUseGrContext {
 public:
  static std::unique_ptr<ScopedUseGrContext> Create(GLRenderer* renderer) {
    // GrContext for filters is created lazily, and may fail if the context
    // is lost.
    // TODO(vmiura,bsalomon): crbug.com/487850 Ensure that
    // ContextProvider::GrContext() does not return NULL.
    if (renderer->output_surface_->context_provider()->GrContext())
      return base::WrapUnique(new ScopedUseGrContext(renderer));
    return nullptr;
  }

  ~ScopedUseGrContext() {
    // Pass context control back to GLrenderer.
    scoped_gpu_raster_ = nullptr;
    renderer_->RestoreGLState();
  }

  GrContext* context() const {
    return renderer_->output_surface_->context_provider()->GrContext();
  }

 private:
  explicit ScopedUseGrContext(GLRenderer* renderer)
      : scoped_gpu_raster_(
            new ScopedGpuRaster(renderer->output_surface_->context_provider())),
        renderer_(renderer) {
    // scoped_gpu_raster_ passes context control to Skia.
  }

  std::unique_ptr<ScopedGpuRaster> scoped_gpu_raster_;
  GLRenderer* renderer_;

  DISALLOW_COPY_AND_ASSIGN(ScopedUseGrContext);
};

struct GLRenderer::PendingAsyncReadPixels {
  PendingAsyncReadPixels() : buffer(0) {}

  std::unique_ptr<CopyOutputRequest> copy_request;
  base::CancelableClosure finished_read_pixels_callback;
  unsigned buffer;

 private:
  DISALLOW_COPY_AND_ASSIGN(PendingAsyncReadPixels);
};

class GLRenderer::SyncQuery {
 public:
  explicit SyncQuery(gpu::gles2::GLES2Interface* gl)
      : gl_(gl), query_id_(0u), is_pending_(false), weak_ptr_factory_(this) {
    gl_->GenQueriesEXT(1, &query_id_);
  }
  virtual ~SyncQuery() { gl_->DeleteQueriesEXT(1, &query_id_); }

  scoped_refptr<ResourceProvider::Fence> Begin() {
    DCHECK(!IsPending());
    // Invalidate weak pointer held by old fence.
    weak_ptr_factory_.InvalidateWeakPtrs();
    // Note: In case the set of drawing commands issued before End() do not
    // depend on the query, defer BeginQueryEXT call until Set() is called and
    // query is required.
    return make_scoped_refptr<ResourceProvider::Fence>(
        new Fence(weak_ptr_factory_.GetWeakPtr()));
  }

  void Set() {
    if (is_pending_)
      return;

    // Note: BeginQueryEXT on GL_COMMANDS_COMPLETED_CHROMIUM is effectively a
    // noop relative to GL, so it doesn't matter where it happens but we still
    // make sure to issue this command when Set() is called (prior to issuing
    // any drawing commands that depend on query), in case some future extension
    // can take advantage of this.
    gl_->BeginQueryEXT(GL_COMMANDS_COMPLETED_CHROMIUM, query_id_);
    is_pending_ = true;
  }

  void End() {
    if (!is_pending_)
      return;

    gl_->EndQueryEXT(GL_COMMANDS_COMPLETED_CHROMIUM);
  }

  bool IsPending() {
    if (!is_pending_)
      return false;

    unsigned result_available = 1;
    gl_->GetQueryObjectuivEXT(
        query_id_, GL_QUERY_RESULT_AVAILABLE_EXT, &result_available);
    is_pending_ = !result_available;
    return is_pending_;
  }

  void Wait() {
    if (!is_pending_)
      return;

    unsigned result = 0;
    gl_->GetQueryObjectuivEXT(query_id_, GL_QUERY_RESULT_EXT, &result);
    is_pending_ = false;
  }

 private:
  class Fence : public ResourceProvider::Fence {
   public:
    explicit Fence(base::WeakPtr<GLRenderer::SyncQuery> query)
        : query_(query) {}

    // Overridden from ResourceProvider::Fence:
    void Set() override {
      DCHECK(query_);
      query_->Set();
    }
    bool HasPassed() override { return !query_ || !query_->IsPending(); }
    void Wait() override {
      if (query_)
        query_->Wait();
    }

   private:
    ~Fence() override {}

    base::WeakPtr<SyncQuery> query_;

    DISALLOW_COPY_AND_ASSIGN(Fence);
  };

  gpu::gles2::GLES2Interface* gl_;
  unsigned query_id_;
  bool is_pending_;
  base::WeakPtrFactory<SyncQuery> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(SyncQuery);
};

std::unique_ptr<GLRenderer> GLRenderer::Create(
    RendererClient* client,
    const RendererSettings* settings,
    OutputSurface* output_surface,
    ResourceProvider* resource_provider,
    TextureMailboxDeleter* texture_mailbox_deleter,
    int highp_threshold_min) {
  return base::WrapUnique(
      new GLRenderer(client, settings, output_surface, resource_provider,
                     texture_mailbox_deleter, highp_threshold_min));
}

GLRenderer::GLRenderer(RendererClient* client,
                       const RendererSettings* settings,
                       OutputSurface* output_surface,
                       ResourceProvider* resource_provider,
                       TextureMailboxDeleter* texture_mailbox_deleter,
                       int highp_threshold_min)
    : DirectRenderer(client, settings, output_surface, resource_provider),
      offscreen_framebuffer_id_(0),
      shared_geometry_quad_(QuadVertexRect()),
      gl_(output_surface->context_provider()->ContextGL()),
      context_support_(output_surface->context_provider()->ContextSupport()),
      texture_mailbox_deleter_(texture_mailbox_deleter),
      is_backbuffer_discarded_(false),
      is_scissor_enabled_(false),
      scissor_rect_needs_reset_(true),
      stencil_shadow_(false),
      blend_shadow_(false),
      highp_threshold_min_(highp_threshold_min),
      highp_threshold_cache_(0),
      use_sync_query_(false),
      gl_composited_texture_quad_border_(
          settings->gl_composited_texture_quad_border),
      bound_geometry_(NO_BINDING) {
  DCHECK(gl_);
  DCHECK(context_support_);

  const auto& context_caps =
      output_surface_->context_provider()->ContextCapabilities();

  capabilities_.using_partial_swap =
      settings_->partial_swap_enabled && context_caps.post_sub_buffer;
  capabilities_.allow_empty_swap =
      capabilities_.using_partial_swap || context_caps.commit_overlay_planes;

  DCHECK(!context_caps.iosurface || context_caps.texture_rectangle);

  capabilities_.using_egl_image = context_caps.egl_image_external;

  capabilities_.max_texture_size = resource_provider_->max_texture_size();
  capabilities_.best_texture_format = resource_provider_->best_texture_format();

  // The updater can access textures while the GLRenderer is using them.
  capabilities_.allow_partial_texture_updates = true;

  capabilities_.using_image = context_caps.image;

  capabilities_.using_discard_framebuffer = context_caps.discard_framebuffer;

  capabilities_.allow_rasterize_on_demand = true;

  // If MSAA is slow, we want this renderer to behave as though MSAA is not
  // available. Set samples to 0 to achieve this.
  if (context_caps.msaa_is_slow)
    capabilities_.max_msaa_samples = 0;
  else
    capabilities_.max_msaa_samples = context_caps.max_samples;

  use_sync_query_ = context_caps.sync_query;
  use_blend_equation_advanced_ = context_caps.blend_equation_advanced;
  use_blend_equation_advanced_coherent_ =
      context_caps.blend_equation_advanced_coherent;

  InitializeSharedObjects();
}

GLRenderer::~GLRenderer() {
  while (!pending_async_read_pixels_.empty()) {
    PendingAsyncReadPixels* pending_read =
        pending_async_read_pixels_.back().get();
    pending_read->finished_read_pixels_callback.Cancel();
    pending_async_read_pixels_.pop_back();
  }

  CleanupSharedObjects();
}

const RendererCapabilitiesImpl& GLRenderer::Capabilities() const {
  return capabilities_;
}

void GLRenderer::DidChangeVisibility() {
  EnforceMemoryPolicy();

  // If we are not visible, we ask the context to aggressively free resources.
  context_support_->SetAggressivelyFreeResources(!visible());
}

void GLRenderer::ReleaseRenderPassTextures() { render_pass_textures_.clear(); }

void GLRenderer::DiscardPixels() {
  if (!capabilities_.using_discard_framebuffer)
    return;
  bool using_default_framebuffer =
      !current_framebuffer_lock_ &&
      output_surface_->capabilities().uses_default_gl_framebuffer;
  GLenum attachments[] = {static_cast<GLenum>(
      using_default_framebuffer ? GL_COLOR_EXT : GL_COLOR_ATTACHMENT0_EXT)};
  gl_->DiscardFramebufferEXT(
      GL_FRAMEBUFFER, arraysize(attachments), attachments);
}

void GLRenderer::PrepareSurfaceForPass(
    DrawingFrame* frame,
    SurfaceInitializationMode initialization_mode,
    const gfx::Rect& render_pass_scissor) {
  SetViewport();

  switch (initialization_mode) {
    case SURFACE_INITIALIZATION_MODE_PRESERVE:
      EnsureScissorTestDisabled();
      return;
    case SURFACE_INITIALIZATION_MODE_FULL_SURFACE_CLEAR:
      EnsureScissorTestDisabled();
      DiscardPixels();
      ClearFramebuffer(frame);
      break;
    case SURFACE_INITIALIZATION_MODE_SCISSORED_CLEAR:
      SetScissorTestRect(render_pass_scissor);
      ClearFramebuffer(frame);
      break;
  }
}

void GLRenderer::ClearFramebuffer(DrawingFrame* frame) {
  // On DEBUG builds, opaque render passes are cleared to blue to easily see
  // regions that were not drawn on the screen.
  if (frame->current_render_pass->has_transparent_background)
    gl_->ClearColor(0, 0, 0, 0);
  else
    gl_->ClearColor(0, 0, 1, 1);

  bool always_clear = false;
#ifndef NDEBUG
  always_clear = true;
#endif
  if (always_clear || frame->current_render_pass->has_transparent_background) {
    GLbitfield clear_bits = GL_COLOR_BUFFER_BIT;
    if (always_clear)
      clear_bits |= GL_STENCIL_BUFFER_BIT;
    gl_->Clear(clear_bits);
  }
}

void GLRenderer::BeginDrawingFrame(DrawingFrame* frame) {
  TRACE_EVENT0("cc", "GLRenderer::BeginDrawingFrame");

  scoped_refptr<ResourceProvider::Fence> read_lock_fence;
  if (use_sync_query_) {
    // Block until oldest sync query has passed if the number of pending queries
    // ever reach kMaxPendingSyncQueries.
    if (pending_sync_queries_.size() >= kMaxPendingSyncQueries) {
      LOG(ERROR) << "Reached limit of pending sync queries.";

      pending_sync_queries_.front()->Wait();
      DCHECK(!pending_sync_queries_.front()->IsPending());
    }

    while (!pending_sync_queries_.empty()) {
      if (pending_sync_queries_.front()->IsPending())
        break;

      available_sync_queries_.push_back(PopFront(&pending_sync_queries_));
    }

    current_sync_query_ = available_sync_queries_.empty()
                              ? base::WrapUnique(new SyncQuery(gl_))
                              : PopFront(&available_sync_queries_);

    read_lock_fence = current_sync_query_->Begin();
  } else {
    read_lock_fence =
        make_scoped_refptr(new ResourceProvider::SynchronousFence(gl_));
  }
  resource_provider_->SetReadLockFence(read_lock_fence.get());

  // Insert WaitSyncTokenCHROMIUM on quad resources prior to drawing the frame,
  // so that drawing can proceed without GL context switching interruptions.
  ResourceProvider* resource_provider = resource_provider_;
  for (const auto& pass : *frame->render_passes_in_draw_order) {
    for (auto* quad : pass->quad_list) {
      for (ResourceId resource_id : quad->resources)
        resource_provider->WaitSyncTokenIfNeeded(resource_id);
    }
  }

  // TODO(enne): Do we need to reinitialize all of this state per frame?
  ReinitializeGLState();
}

void GLRenderer::DoDrawQuad(DrawingFrame* frame,
                            const DrawQuad* quad,
                            const gfx::QuadF* clip_region) {
  DCHECK(quad->rect.Contains(quad->visible_rect));
  if (quad->material != DrawQuad::TEXTURE_CONTENT) {
    FlushTextureQuadCache(SHARED_BINDING);
  }

  switch (quad->material) {
    case DrawQuad::INVALID:
      NOTREACHED();
      break;
    case DrawQuad::DEBUG_BORDER:
      DrawDebugBorderQuad(frame, DebugBorderDrawQuad::MaterialCast(quad));
      break;
    case DrawQuad::PICTURE_CONTENT:
      // PictureDrawQuad should only be used for resourceless software draws.
      NOTREACHED();
      break;
    case DrawQuad::RENDER_PASS:
      DrawRenderPassQuad(frame, RenderPassDrawQuad::MaterialCast(quad),
                         clip_region);
      break;
    case DrawQuad::SOLID_COLOR:
      DrawSolidColorQuad(frame, SolidColorDrawQuad::MaterialCast(quad),
                         clip_region);
      break;
    case DrawQuad::STREAM_VIDEO_CONTENT:
      DrawStreamVideoQuad(frame, StreamVideoDrawQuad::MaterialCast(quad),
                          clip_region);
      break;
    case DrawQuad::SURFACE_CONTENT:
      // Surface content should be fully resolved to other quad types before
      // reaching a direct renderer.
      NOTREACHED();
      break;
    case DrawQuad::TEXTURE_CONTENT:
      EnqueueTextureQuad(frame, TextureDrawQuad::MaterialCast(quad),
                         clip_region);
      break;
    case DrawQuad::TILED_CONTENT:
      DrawTileQuad(frame, TileDrawQuad::MaterialCast(quad), clip_region);
      break;
    case DrawQuad::YUV_VIDEO_CONTENT:
      DrawYUVVideoQuad(frame, YUVVideoDrawQuad::MaterialCast(quad),
                       clip_region);
      break;
  }
}

// This function does not handle 3D sorting right now, since the debug border
// quads are just drawn as their original quads and not in split pieces. This
// results in some debug border quads drawing over foreground quads.
void GLRenderer::DrawDebugBorderQuad(const DrawingFrame* frame,
                                     const DebugBorderDrawQuad* quad) {
  SetBlendEnabled(quad->ShouldDrawWithBlending());

  static float gl_matrix[16];
  const DebugBorderProgram* program = GetDebugBorderProgram();
  DCHECK(program && (program->initialized() || IsContextLost()));
  SetUseProgram(program->program());

  // Use the full quad_rect for debug quads to not move the edges based on
  // partial swaps.
  gfx::Rect layer_rect = quad->rect;
  gfx::Transform render_matrix;
  QuadRectTransform(&render_matrix,
                    quad->shared_quad_state->quad_to_target_transform,
                    gfx::RectF(layer_rect));
  GLRenderer::ToGLMatrix(&gl_matrix[0],
                         frame->projection_matrix * render_matrix);
  gl_->UniformMatrix4fv(program->vertex_shader().matrix_location(), 1, false,
                        &gl_matrix[0]);

  SkColor color = quad->color;
  float alpha = SkColorGetA(color) * (1.0f / 255.0f);

  gl_->Uniform4f(program->fragment_shader().color_location(),
                 (SkColorGetR(color) * (1.0f / 255.0f)) * alpha,
                 (SkColorGetG(color) * (1.0f / 255.0f)) * alpha,
                 (SkColorGetB(color) * (1.0f / 255.0f)) * alpha, alpha);

  gl_->LineWidth(quad->width);

  // The indices for the line are stored in the same array as the triangle
  // indices.
  gl_->DrawElements(GL_LINE_LOOP, 4, GL_UNSIGNED_SHORT, 0);
}

static sk_sp<SkImage> WrapTexture(
    const ResourceProvider::ScopedReadLockGL& lock,
    GrContext* context,
    bool flip_texture) {
  // Wrap a given texture in a Ganesh platform texture.
  GrBackendTextureDesc backend_texture_description;
  GrGLTextureInfo texture_info;
  texture_info.fTarget = lock.target();
  texture_info.fID = lock.texture_id();
  backend_texture_description.fWidth = lock.size().width();
  backend_texture_description.fHeight = lock.size().height();
  backend_texture_description.fConfig = kSkia8888_GrPixelConfig;
  backend_texture_description.fTextureHandle =
      skia::GrGLTextureInfoToGrBackendObject(texture_info);
  backend_texture_description.fOrigin =
      flip_texture ? kBottomLeft_GrSurfaceOrigin : kTopLeft_GrSurfaceOrigin;

  return SkImage::MakeFromTexture(context, backend_texture_description);
}

static sk_sp<SkImage> ApplyImageFilter(
    std::unique_ptr<GLRenderer::ScopedUseGrContext> use_gr_context,
    ResourceProvider* resource_provider,
    const gfx::RectF& src_rect,
    const gfx::RectF& dst_rect,
    const gfx::Vector2dF& scale,
    sk_sp<SkImageFilter> filter,
    const Resource* source_texture_resource,
    SkIPoint* offset,
    SkIRect* subset,
    bool flip_texture) {
  if (!filter || !use_gr_context)
    return nullptr;

  ResourceProvider::ScopedReadLockGL lock(resource_provider,
                                          source_texture_resource->id());

  sk_sp<SkImage> src_image =
      WrapTexture(lock, use_gr_context->context(), flip_texture);

  if (!src_image) {
    TRACE_EVENT_INSTANT0("cc",
                         "ApplyImageFilter wrap background texture failed",
                         TRACE_EVENT_SCOPE_THREAD);
    return nullptr;
  }

  SkMatrix local_matrix;
  local_matrix.setTranslate(-src_rect.x(), -src_rect.y());
  local_matrix.postScale(scale.x(), scale.y());

  SkIRect clip_bounds = gfx::RectFToSkRect(dst_rect).roundOut();
  clip_bounds.offset(-src_rect.x(), -src_rect.y());
  filter = filter->makeWithLocalMatrix(local_matrix);
  SkIRect in_subset = SkIRect::MakeWH(src_rect.width(), src_rect.height());
  sk_sp<SkImage> image = src_image->makeWithFilter(filter.get(), in_subset,
                                                   clip_bounds, subset, offset);

  if (!image || !image->isTextureBacked()) {
    return nullptr;
  }

  // Force a flush of the Skia pipeline before we switch back to the compositor
  // context.
  image->getTextureHandle(true);
  CHECK(image->isTextureBacked());
  return image;
}

bool GLRenderer::CanApplyBlendModeUsingBlendFunc(SkXfermode::Mode blend_mode) {
  return use_blend_equation_advanced_ ||
         blend_mode == SkXfermode::kScreen_Mode ||
         blend_mode == SkXfermode::kSrcOver_Mode;
}

void GLRenderer::ApplyBlendModeUsingBlendFunc(SkXfermode::Mode blend_mode) {
  DCHECK(CanApplyBlendModeUsingBlendFunc(blend_mode));

  // Any modes set here must be reset in RestoreBlendFuncToDefault
  if (use_blend_equation_advanced_) {
    GLenum equation = GL_FUNC_ADD;

    switch (blend_mode) {
      case SkXfermode::kScreen_Mode:
        equation = GL_SCREEN_KHR;
        break;
      case SkXfermode::kOverlay_Mode:
        equation = GL_OVERLAY_KHR;
        break;
      case SkXfermode::kDarken_Mode:
        equation = GL_DARKEN_KHR;
        break;
      case SkXfermode::kLighten_Mode:
        equation = GL_LIGHTEN_KHR;
        break;
      case SkXfermode::kColorDodge_Mode:
        equation = GL_COLORDODGE_KHR;
        break;
      case SkXfermode::kColorBurn_Mode:
        equation = GL_COLORBURN_KHR;
        break;
      case SkXfermode::kHardLight_Mode:
        equation = GL_HARDLIGHT_KHR;
        break;
      case SkXfermode::kSoftLight_Mode:
        equation = GL_SOFTLIGHT_KHR;
        break;
      case SkXfermode::kDifference_Mode:
        equation = GL_DIFFERENCE_KHR;
        break;
      case SkXfermode::kExclusion_Mode:
        equation = GL_EXCLUSION_KHR;
        break;
      case SkXfermode::kMultiply_Mode:
        equation = GL_MULTIPLY_KHR;
        break;
      case SkXfermode::kHue_Mode:
        equation = GL_HSL_HUE_KHR;
        break;
      case SkXfermode::kSaturation_Mode:
        equation = GL_HSL_SATURATION_KHR;
        break;
      case SkXfermode::kColor_Mode:
        equation = GL_HSL_COLOR_KHR;
        break;
      case SkXfermode::kLuminosity_Mode:
        equation = GL_HSL_LUMINOSITY_KHR;
        break;
      default:
        return;
    }

    gl_->BlendEquation(equation);
  } else {
    if (blend_mode == SkXfermode::kScreen_Mode) {
      gl_->BlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE);
    }
  }
}

void GLRenderer::RestoreBlendFuncToDefault(SkXfermode::Mode blend_mode) {
  if (blend_mode == SkXfermode::kSrcOver_Mode)
    return;

  if (use_blend_equation_advanced_) {
    gl_->BlendEquation(GL_FUNC_ADD);
  } else {
    gl_->BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  }
}

bool GLRenderer::ShouldApplyBackgroundFilters(const RenderPassDrawQuad* quad) {
  if (quad->background_filters.IsEmpty())
    return false;

  // TODO(hendrikw): Look into allowing background filters to see pixels from
  // other render targets.  See crbug.com/314867.

  return true;
}

// This takes a gfx::Rect and a clip region quad in the same space,
// and returns a quad with the same proportions in the space -0.5->0.5.
bool GetScaledRegion(const gfx::Rect& rect,
                     const gfx::QuadF* clip,
                     gfx::QuadF* scaled_region) {
  if (!clip)
    return false;

  gfx::PointF p1(((clip->p1().x() - rect.x()) / rect.width()) - 0.5f,
                 ((clip->p1().y() - rect.y()) / rect.height()) - 0.5f);
  gfx::PointF p2(((clip->p2().x() - rect.x()) / rect.width()) - 0.5f,
                 ((clip->p2().y() - rect.y()) / rect.height()) - 0.5f);
  gfx::PointF p3(((clip->p3().x() - rect.x()) / rect.width()) - 0.5f,
                 ((clip->p3().y() - rect.y()) / rect.height()) - 0.5f);
  gfx::PointF p4(((clip->p4().x() - rect.x()) / rect.width()) - 0.5f,
                 ((clip->p4().y() - rect.y()) / rect.height()) - 0.5f);
  *scaled_region = gfx::QuadF(p1, p2, p3, p4);
  return true;
}

// This takes a gfx::Rect and a clip region quad in the same space,
// and returns the proportional uv's in the space 0->1.
bool GetScaledUVs(const gfx::Rect& rect, const gfx::QuadF* clip, float uvs[8]) {
  if (!clip)
    return false;

  uvs[0] = ((clip->p1().x() - rect.x()) / rect.width());
  uvs[1] = ((clip->p1().y() - rect.y()) / rect.height());
  uvs[2] = ((clip->p2().x() - rect.x()) / rect.width());
  uvs[3] = ((clip->p2().y() - rect.y()) / rect.height());
  uvs[4] = ((clip->p3().x() - rect.x()) / rect.width());
  uvs[5] = ((clip->p3().y() - rect.y()) / rect.height());
  uvs[6] = ((clip->p4().x() - rect.x()) / rect.width());
  uvs[7] = ((clip->p4().y() - rect.y()) / rect.height());
  return true;
}

gfx::Rect GLRenderer::GetBackdropBoundingBoxForRenderPassQuad(
    DrawingFrame* frame,
    const RenderPassDrawQuad* quad,
    const gfx::Transform& contents_device_transform,
    const gfx::QuadF* clip_region,
    bool use_aa) {
  gfx::QuadF scaled_region;
  if (!GetScaledRegion(quad->rect, clip_region, &scaled_region)) {
    scaled_region = SharedGeometryQuad().BoundingBox();
  }

  gfx::Rect backdrop_rect = gfx::ToEnclosingRect(MathUtil::MapClippedRect(
      contents_device_transform, scaled_region.BoundingBox()));

  if (ShouldApplyBackgroundFilters(quad)) {
    SkMatrix matrix;
    matrix.setScale(quad->filters_scale.x(), quad->filters_scale.y());
    if (FlippedFramebuffer(frame)) {
      // TODO(jbroman): This probably isn't the right way to account for this.
      // Probably some combination of frame->projection_matrix,
      // frame->window_matrix and contents_device_transform?
      matrix.postScale(1, -1);
    }
    backdrop_rect =
        quad->background_filters.MapRectReverse(backdrop_rect, matrix);
  }

  if (!backdrop_rect.IsEmpty() && use_aa) {
    const int kOutsetForAntialiasing = 1;
    backdrop_rect.Inset(-kOutsetForAntialiasing, -kOutsetForAntialiasing);
  }

  if (!quad->filters.IsEmpty()) {
    // If we have filters, grab an extra one-pixel border around the
    // background, so texture edge clamping gives us a transparent border
    // in case the filter expands the result.
    backdrop_rect.Inset(-1, -1, -1, -1);
  }

  backdrop_rect.Intersect(MoveFromDrawToWindowSpace(
      frame, frame->current_render_pass->output_rect));
  return backdrop_rect;
}

std::unique_ptr<ScopedResource> GLRenderer::GetBackdropTexture(
    const gfx::Rect& bounding_rect) {
  std::unique_ptr<ScopedResource> device_background_texture =
      ScopedResource::Create(resource_provider_);
  // CopyTexImage2D fails when called on a texture having immutable storage.
  device_background_texture->Allocate(
      bounding_rect.size(), ResourceProvider::TEXTURE_HINT_DEFAULT,
      resource_provider_->best_texture_format());
  {
    ResourceProvider::ScopedWriteLockGL lock(
        resource_provider_, device_background_texture->id(), false);
    GetFramebufferTexture(lock.texture_id(), bounding_rect);
  }
  return device_background_texture;
}

sk_sp<SkImage> GLRenderer::ApplyBackgroundFilters(
    const RenderPassDrawQuad* quad,
    ScopedResource* background_texture,
    const gfx::RectF& rect) {
  DCHECK(ShouldApplyBackgroundFilters(quad));
  auto use_gr_context = ScopedUseGrContext::Create(this);
  sk_sp<SkImageFilter> filter = RenderSurfaceFilters::BuildImageFilter(
      quad->background_filters, gfx::SizeF(background_texture->size()));

  // TODO(senorblanco): background filters should be moved to the
  // makeWithFilter fast-path, and go back to calling ApplyImageFilter().
  // See http://crbug.com/613233.
  if (!filter || !use_gr_context)
    return nullptr;

  ResourceProvider::ScopedReadLockGL lock(resource_provider_,
                                          background_texture->id());

  bool flip_texture = true;
  sk_sp<SkImage> src_image =
      WrapTexture(lock, use_gr_context->context(), flip_texture);
  if (!src_image) {
    TRACE_EVENT_INSTANT0(
        "cc", "ApplyBackgroundFilters wrap background texture failed",
        TRACE_EVENT_SCOPE_THREAD);
    return nullptr;
  }

  // Create surface to draw into.
  SkImageInfo dst_info =
      SkImageInfo::MakeN32Premul(rect.width(), rect.height());
  sk_sp<SkSurface> surface = SkSurface::MakeRenderTarget(
      use_gr_context->context(), SkBudgeted::kYes, dst_info);
  if (!surface) {
    TRACE_EVENT_INSTANT0("cc",
                         "ApplyBackgroundFilters surface allocation failed",
                         TRACE_EVENT_SCOPE_THREAD);
    return nullptr;
  }

  SkMatrix local_matrix;
  local_matrix.setScale(quad->filters_scale.x(), quad->filters_scale.y());

  SkPaint paint;
  paint.setImageFilter(filter->makeWithLocalMatrix(local_matrix));
  surface->getCanvas()->translate(-rect.x(), -rect.y());
  surface->getCanvas()->drawImage(src_image, rect.x(), rect.y(), &paint);
  // Flush the drawing before source texture read lock goes out of scope.
  // Skia API does not guarantee that when the SkImage goes out of scope,
  // its externally referenced resources would force the rendering to be
  // flushed.
  surface->getCanvas()->flush();
  sk_sp<SkImage> image = surface->makeImageSnapshot();
  if (!image || !image->isTextureBacked()) {
    return nullptr;
  }

  return image;
}

// Map device space quad to local space. Device_transform has no 3d
// component since it was flattened, so we don't need to project.  We should
// have already checked that the transform was uninvertible before this call.
gfx::QuadF MapQuadToLocalSpace(const gfx::Transform& device_transform,
                               const gfx::QuadF& device_quad) {
  gfx::Transform inverse_device_transform(gfx::Transform::kSkipInitialization);
  DCHECK(device_transform.IsInvertible());
  bool did_invert = device_transform.GetInverse(&inverse_device_transform);
  DCHECK(did_invert);
  bool clipped = false;
  gfx::QuadF local_quad =
      MathUtil::MapQuad(inverse_device_transform, device_quad, &clipped);
  // We should not DCHECK(!clipped) here, because anti-aliasing inflation may
  // cause device_quad to become clipped. To our knowledge this scenario does
  // not need to be handled differently than the unclipped case.
  return local_quad;
}

const TileDrawQuad* GLRenderer::CanPassBeDrawnDirectly(const RenderPass* pass) {
  // Can only collapse a single tile quad.
  if (pass->quad_list.size() != 1)
    return nullptr;
  // If we need copy requests, then render pass has to exist.
  if (!pass->copy_requests.empty())
    return nullptr;

  const DrawQuad* quad = *pass->quad_list.BackToFrontBegin();
  // Hack: this could be supported by concatenating transforms, but
  // in practice if there is one quad, it is at the origin of the render pass.
  if (!quad->shared_quad_state->quad_to_target_transform.IsIdentity())
    return nullptr;
  // The quad is expected to be the entire layer so that AA edges are correct.
  if (gfx::Rect(quad->shared_quad_state->quad_layer_bounds) != quad->rect)
    return nullptr;
  if (quad->material != DrawQuad::TILED_CONTENT)
    return nullptr;

  const TileDrawQuad* tile_quad = TileDrawQuad::MaterialCast(quad);
  // Hack: this could be supported by passing in a subrectangle to draw
  // render pass, although in practice if there is only one quad there
  // will be no border texels on the input.
  if (tile_quad->tex_coord_rect != gfx::RectF(tile_quad->rect))
    return nullptr;
  // Tile quad features not supported in render pass shaders.
  if (tile_quad->swizzle_contents || tile_quad->nearest_neighbor)
    return nullptr;
  // BUG=skia:3868, Skia currently doesn't support texture rectangle inputs.
  // See also the DCHECKs about GL_TEXTURE_2D in DrawRenderPassQuad.
  GLenum target =
      resource_provider_->GetResourceTextureTarget(tile_quad->resource_id());
  if (target != GL_TEXTURE_2D)
    return nullptr;
#if defined(OS_MACOSX)
  // On Macs, this path can sometimes lead to all black output.
  // TODO(enne): investigate this and remove this hack.
  return nullptr;
#endif

  return tile_quad;
}

void GLRenderer::DrawRenderPassQuad(DrawingFrame* frame,
                                    const RenderPassDrawQuad* quad,
                                    const gfx::QuadF* clip_region) {
  auto bypass = render_pass_bypass_quads_.find(quad->render_pass_id);
  DrawRenderPassDrawQuadParams params;
  params.quad = quad;
  params.frame = frame;
  params.clip_region = clip_region;
  params.window_matrix = frame->window_matrix;
  params.projection_matrix = frame->projection_matrix;
  if (bypass != render_pass_bypass_quads_.end()) {
    TileDrawQuad* tile_quad = &bypass->second;
    // RGBA_8888 here is arbitrary and unused.
    Resource tile_resource(tile_quad->resource_id(), tile_quad->texture_size,
                           ResourceFormat::RGBA_8888);
    // The projection matrix used by GLRenderer has a flip.  As tile texture
    // inputs are oriented opposite to framebuffer outputs, don't flip via
    // texture coords and let the projection matrix naturallyd o it.
    params.flip_texture = false;
    params.contents_texture = &tile_resource;
    DrawRenderPassQuadInternal(&params);
  } else {
    ScopedResource* contents_texture =
        render_pass_textures_[quad->render_pass_id].get();
    DCHECK(contents_texture);
    DCHECK(contents_texture->id());
    // See above comments about texture flipping.  When the input is a
    // render pass, it needs to an extra flip to be oriented correctly.
    params.flip_texture = true;
    params.contents_texture = contents_texture;
    DrawRenderPassQuadInternal(&params);
  }
}

void GLRenderer::DrawRenderPassQuadInternal(
    DrawRenderPassDrawQuadParams* params) {
  if (!InitializeRPDQParameters(params))
    return;
  UpdateRPDQShadersForBlending(params);
  if (!UpdateRPDQWithSkiaFilters(params))
    return;
  UseRenderPass(params->frame, params->frame->current_render_pass);
  SetViewport();
  UpdateRPDQTexturesForSampling(params);
  UpdateRPDQBlendMode(params);
  ChooseRPDQProgram(params);
  UpdateRPDQUniforms(params);
  DrawRPDQ(*params);
}

bool GLRenderer::InitializeRPDQParameters(
    DrawRenderPassDrawQuadParams* params) {
  const RenderPassDrawQuad* quad = params->quad;
  SkMatrix scale_matrix;
  scale_matrix.setScale(quad->filters_scale.x(), quad->filters_scale.y());
  gfx::Rect dst_rect = quad->filters.MapRect(quad->rect, scale_matrix);
  params->dst_rect.SetRect(static_cast<float>(dst_rect.x()),
                           static_cast<float>(dst_rect.y()),
                           static_cast<float>(dst_rect.width()),
                           static_cast<float>(dst_rect.height()));
  gfx::Transform quad_rect_matrix;
  QuadRectTransform(&quad_rect_matrix,
                    quad->shared_quad_state->quad_to_target_transform,
                    params->dst_rect);
  params->contents_device_transform =
      params->window_matrix * params->projection_matrix * quad_rect_matrix;
  params->contents_device_transform.FlattenTo2d();

  // Can only draw surface if device matrix is invertible.
  if (!params->contents_device_transform.IsInvertible())
    return false;

  params->surface_quad = SharedGeometryQuad();

  gfx::QuadF device_layer_quad;
  if (settings_->allow_antialiasing) {
    bool clipped = false;
    device_layer_quad = MathUtil::MapQuad(params->contents_device_transform,
                                          params->surface_quad, &clipped);
    params->use_aa = ShouldAntialiasQuad(device_layer_quad, clipped,
                                         settings_->force_antialiasing);
  }

  const gfx::QuadF* aa_quad = params->use_aa ? &device_layer_quad : nullptr;
  SetupRenderPassQuadForClippingAndAntialiasing(
      params->contents_device_transform, quad, aa_quad, params->clip_region,
      &params->surface_quad, params->edge);

  return true;
}

void GLRenderer::UpdateRPDQShadersForBlending(
    DrawRenderPassDrawQuadParams* params) {
  const RenderPassDrawQuad* quad = params->quad;
  SkXfermode::Mode blend_mode = quad->shared_quad_state->blend_mode;
  params->use_shaders_for_blending =
      !CanApplyBlendModeUsingBlendFunc(blend_mode) ||
      ShouldApplyBackgroundFilters(quad) ||
      settings_->force_blending_with_shaders;

  if (params->use_shaders_for_blending) {
    DCHECK(params->frame);
    // Compute a bounding box around the pixels that will be visible through
    // the quad.
    params->background_rect = GetBackdropBoundingBoxForRenderPassQuad(
        params->frame, quad, params->contents_device_transform,
        params->clip_region, params->use_aa);

    if (!params->background_rect.IsEmpty()) {
      // The pixels from the filtered background should completely replace the
      // current pixel values.
      if (blend_enabled())
        SetBlendEnabled(false);

      // Read the pixels in the bounding box into a buffer R.
      // This function allocates a texture, which should contribute to the
      // amount of memory used by render surfaces:
      // LayerTreeHost::CalculateMemoryForRenderSurfaces.
      params->background_texture = GetBackdropTexture(params->background_rect);

      if (ShouldApplyBackgroundFilters(quad) && params->background_texture) {
        // Apply the background filters to R, so that it is applied in the
        // pixels' coordinate space.
        params->background_image =
            ApplyBackgroundFilters(quad, params->background_texture.get(),
                                   gfx::RectF(params->background_rect));
        if (params->background_image) {
          params->background_image_id =
              skia::GrBackendObjectToGrGLTextureInfo(
                  params->background_image->getTextureHandle(true))
                  ->fID;
          DCHECK(params->background_image_id);
        }
      }
    }

    if (!params->background_texture) {
      // Something went wrong with reading the backdrop.
      DCHECK(!params->background_image_id);
      params->use_shaders_for_blending = false;
    } else if (params->background_image_id) {
      // Reset original background texture if there is not any mask
      if (!quad->mask_resource_id())
        params->background_texture.reset();
    } else if (CanApplyBlendModeUsingBlendFunc(blend_mode) &&
               ShouldApplyBackgroundFilters(quad)) {
      // Something went wrong with applying background filters to the backdrop.
      params->use_shaders_for_blending = false;
      params->background_texture.reset();
    }
  }
  // Need original background texture for mask?
  params->mask_for_background =
      params->background_texture &&   // Have original background texture
      params->background_image_id &&  // Have filtered background texture
      quad->mask_resource_id();       // Have mask texture
  DCHECK_EQ(params->background_texture || params->background_image_id,
            params->use_shaders_for_blending);
}

bool GLRenderer::UpdateRPDQWithSkiaFilters(
    DrawRenderPassDrawQuadParams* params) {
  const RenderPassDrawQuad* quad = params->quad;
  // TODO(senorblanco): Cache this value so that we don't have to do it for both
  // the surface and its replica.  Apply filters to the contents texture.
  if (!quad->filters.IsEmpty()) {
    sk_sp<SkImageFilter> filter = RenderSurfaceFilters::BuildImageFilter(
        quad->filters, gfx::SizeF(params->contents_texture->size()));
    if (filter) {
      SkColorFilter* colorfilter_rawptr = NULL;
      filter->asColorFilter(&colorfilter_rawptr);
      sk_sp<SkColorFilter> cf(colorfilter_rawptr);

      if (cf && cf->asColorMatrix(params->color_matrix)) {
        // We have a color matrix at the root of the filter DAG; apply it
        // locally in the compositor and process the rest of the DAG (if any)
        // in Skia.
        params->use_color_matrix = true;
        filter = sk_ref_sp(filter->getInput(0));
      }
      if (filter) {
        gfx::Rect clip_rect = quad->shared_quad_state->clip_rect;
        if (clip_rect.IsEmpty()) {
          clip_rect = current_draw_rect_;
        }
        gfx::Transform transform =
            quad->shared_quad_state->quad_to_target_transform;
        gfx::QuadF clip_quad = gfx::QuadF(gfx::RectF(clip_rect));
        gfx::QuadF local_clip = MapQuadToLocalSpace(transform, clip_quad);
        params->dst_rect.Intersect(local_clip.BoundingBox());
        // If we've been fully clipped out (by crop rect or clipping), there's
        // nothing to draw.
        if (params->dst_rect.IsEmpty()) {
          return false;
        }
        SkIPoint offset;
        SkIRect subset;
        gfx::RectF src_rect(quad->rect);
        params->filter_image = ApplyImageFilter(
            ScopedUseGrContext::Create(this), resource_provider_, src_rect,
            params->dst_rect, quad->filters_scale, std::move(filter),
            params->contents_texture, &offset, &subset, params->flip_texture);
        if (!params->filter_image)
          return false;
        params->dst_rect =
            gfx::RectF(src_rect.x() + offset.fX, src_rect.y() + offset.fY,
                       subset.width(), subset.height());
        params->src_offset.SetPoint(subset.x(), subset.y());
      }
    }
  }
  return true;
}

void GLRenderer::UpdateRPDQTexturesForSampling(
    DrawRenderPassDrawQuadParams* params) {
  if (params->quad->mask_resource_id()) {
    params->mask_resource_lock.reset(new ResourceProvider::ScopedSamplerGL(
        resource_provider_, params->quad->mask_resource_id(), GL_TEXTURE1,
        GL_LINEAR));
  }

  if (params->filter_image) {
    GLuint filter_image_id = skia::GrBackendObjectToGrGLTextureInfo(
                                 params->filter_image->getTextureHandle(true))
                                 ->fID;
    DCHECK(filter_image_id);
    DCHECK_EQ(GL_TEXTURE0, GetActiveTextureUnit(gl_));
    gl_->BindTexture(GL_TEXTURE_2D, filter_image_id);
    gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    params->source_needs_flip = params->filter_image->getTexture()->origin() ==
                                kBottomLeft_GrSurfaceOrigin;
  } else {
    params->contents_resource_lock =
        base::WrapUnique(new ResourceProvider::ScopedSamplerGL(
            resource_provider_, params->contents_texture->id(), GL_LINEAR));
    DCHECK_EQ(static_cast<GLenum>(GL_TEXTURE_2D),
              params->contents_resource_lock->target());
    params->source_needs_flip = params->flip_texture;
  }
}

void GLRenderer::UpdateRPDQBlendMode(DrawRenderPassDrawQuadParams* params) {
  SkXfermode::Mode blend_mode = params->quad->shared_quad_state->blend_mode;
  SetBlendEnabled(!params->use_shaders_for_blending &&
                  (params->quad->ShouldDrawWithBlending() ||
                   !IsDefaultBlendMode(blend_mode)));
  if (!params->use_shaders_for_blending) {
    if (!use_blend_equation_advanced_coherent_ && use_blend_equation_advanced_)
      gl_->BlendBarrierKHR();

    ApplyBlendModeUsingBlendFunc(blend_mode);
  }
}

void GLRenderer::ChooseRPDQProgram(DrawRenderPassDrawQuadParams* params) {
  TexCoordPrecision tex_coord_precision = TexCoordPrecisionRequired(
      gl_, &highp_threshold_cache_, highp_threshold_min_,
      params->quad->shared_quad_state->visible_quad_layer_rect.bottom_right());

  BlendMode shader_blend_mode =
      params->use_shaders_for_blending
          ? BlendModeFromSkXfermode(params->quad->shared_quad_state->blend_mode)
          : BLEND_MODE_NONE;

  unsigned mask_texture_id = 0;
  SamplerType mask_sampler = SAMPLER_TYPE_NA;
  if (params->mask_resource_lock) {
    mask_texture_id = params->mask_resource_lock->texture_id();
    mask_sampler =
        SamplerTypeFromTextureTarget(params->mask_resource_lock->target());
  }
  bool mask_for_background = params->mask_for_background;

  if (params->use_aa && mask_texture_id && !params->use_color_matrix) {
    const RenderPassMaskProgramAA* program = GetRenderPassMaskProgramAA(
        tex_coord_precision, mask_sampler,
        shader_blend_mode, mask_for_background);
    SetUseProgram(program->program());
    program->vertex_shader().FillLocations(&params->locations);
    program->fragment_shader().FillLocations(&params->locations);
    gl_->Uniform1i(params->locations.sampler, 0);
  } else if (!params->use_aa && mask_texture_id && !params->use_color_matrix) {
    const RenderPassMaskProgram* program = GetRenderPassMaskProgram(
        tex_coord_precision, mask_sampler,
        shader_blend_mode, mask_for_background);
    SetUseProgram(program->program());
    program->vertex_shader().FillLocations(&params->locations);
    program->fragment_shader().FillLocations(&params->locations);
    gl_->Uniform1i(params->locations.sampler, 0);
  } else if (params->use_aa && !mask_texture_id && !params->use_color_matrix) {
    const RenderPassProgramAA* program =
        GetRenderPassProgramAA(tex_coord_precision, shader_blend_mode);
    SetUseProgram(program->program());
    program->vertex_shader().FillLocations(&params->locations);
    program->fragment_shader().FillLocations(&params->locations);
    gl_->Uniform1i(params->locations.sampler, 0);
  } else if (params->use_aa && mask_texture_id && params->use_color_matrix) {
    const RenderPassMaskColorMatrixProgramAA* program =
        GetRenderPassMaskColorMatrixProgramAA(
            tex_coord_precision, mask_sampler,
            shader_blend_mode, mask_for_background);
    SetUseProgram(program->program());
    program->vertex_shader().FillLocations(&params->locations);
    program->fragment_shader().FillLocations(&params->locations);
    gl_->Uniform1i(params->locations.sampler, 0);
  } else if (params->use_aa && !mask_texture_id && params->use_color_matrix) {
    const RenderPassColorMatrixProgramAA* program =
        GetRenderPassColorMatrixProgramAA(tex_coord_precision,
                                          shader_blend_mode);
    SetUseProgram(program->program());
    program->vertex_shader().FillLocations(&params->locations);
    program->fragment_shader().FillLocations(&params->locations);
    gl_->Uniform1i(params->locations.sampler, 0);
  } else if (!params->use_aa && mask_texture_id && params->use_color_matrix) {
    const RenderPassMaskColorMatrixProgram* program =
        GetRenderPassMaskColorMatrixProgram(
            tex_coord_precision, mask_sampler,
            shader_blend_mode, mask_for_background);
    SetUseProgram(program->program());
    program->vertex_shader().FillLocations(&params->locations);
    program->fragment_shader().FillLocations(&params->locations);
    gl_->Uniform1i(params->locations.sampler, 0);
  } else if (!params->use_aa && !mask_texture_id && params->use_color_matrix) {
    const RenderPassColorMatrixProgram* program =
        GetRenderPassColorMatrixProgram(tex_coord_precision, shader_blend_mode);
    SetUseProgram(program->program());
    program->vertex_shader().FillLocations(&params->locations);
    program->fragment_shader().FillLocations(&params->locations);
    gl_->Uniform1i(params->locations.sampler, 0);
  } else {
    const RenderPassProgram* program =
        GetRenderPassProgram(tex_coord_precision, shader_blend_mode);
    SetUseProgram(program->program());
    program->vertex_shader().FillLocations(&params->locations);
    program->fragment_shader().FillLocations(&params->locations);
    gl_->Uniform1i(params->locations.sampler, 0);
  }
}

void GLRenderer::UpdateRPDQUniforms(DrawRenderPassDrawQuadParams* params) {
  ShaderLocations& locations = params->locations;
  gfx::RectF tex_rect(params->src_offset.x(), params->src_offset.y(),
                      params->dst_rect.width(), params->dst_rect.height());
  gfx::Size texture_size;
  if (params->filter_image) {
    texture_size.set_width(params->filter_image->width());
    texture_size.set_height(params->filter_image->height());
  } else {
    texture_size = params->contents_texture->size();
  }
  tex_rect.Scale(1.0f / texture_size.width(), 1.0f / texture_size.height());

  DCHECK(locations.tex_transform != -1 || IsContextLost());
  if (params->source_needs_flip) {
    // Flip the content vertically in the shader, as the RenderPass input
    // texture is already oriented the same way as the framebuffer, but the
    // projection transform does a flip.
    gl_->Uniform4f(locations.tex_transform, tex_rect.x(), 1.0f - tex_rect.y(),
                   tex_rect.width(), -tex_rect.height());
  } else {
    // Tile textures are oriented opposite the framebuffer, so can use
    // the projection transform to do the flip.
    gl_->Uniform4f(locations.tex_transform, tex_rect.x(), tex_rect.y(),
                   tex_rect.width(), tex_rect.height());
  }

  GLint last_texture_unit = 0;
  if (locations.mask_sampler != -1) {
    DCHECK(params->mask_resource_lock);
    DCHECK_NE(locations.mask_tex_coord_scale, 1);
    DCHECK_NE(locations.mask_tex_coord_offset, 1);
    gl_->Uniform1i(locations.mask_sampler, 1);

    gfx::RectF mask_uv_rect = params->quad->MaskUVRect();
    if (SamplerTypeFromTextureTarget(params->mask_resource_lock->target()) !=
        SAMPLER_TYPE_2D) {
      mask_uv_rect.Scale(params->quad->mask_texture_size.width(),
                         params->quad->mask_texture_size.height());
    }
    if (params->source_needs_flip) {
      // Mask textures are oriented vertically flipped relative to the
      // framebuffer and the RenderPass contents texture, so we flip the tex
      // coords from the RenderPass texture to find the mask texture coords.
      gl_->Uniform2f(
          locations.mask_tex_coord_offset, mask_uv_rect.x(),
          mask_uv_rect.height() / tex_rect.height() + mask_uv_rect.y());
      gl_->Uniform2f(locations.mask_tex_coord_scale,
                     mask_uv_rect.width() / tex_rect.width(),
                     -mask_uv_rect.height() / tex_rect.height());
    } else {
      // Tile textures are oriented the same way as mask textures.
      gl_->Uniform2f(locations.mask_tex_coord_offset, mask_uv_rect.x(),
                     mask_uv_rect.y());
      gl_->Uniform2f(locations.mask_tex_coord_scale,
                     mask_uv_rect.width() / tex_rect.width(),
                     mask_uv_rect.height() / tex_rect.height());
    }

    last_texture_unit = 1;
  }

  if (locations.edge != -1)
    gl_->Uniform3fv(locations.edge, 8, params->edge);

  if (locations.viewport != -1) {
    float viewport[4] = {
        static_cast<float>(current_window_space_viewport_.x()),
        static_cast<float>(current_window_space_viewport_.y()),
        static_cast<float>(current_window_space_viewport_.width()),
        static_cast<float>(current_window_space_viewport_.height()),
    };
    gl_->Uniform4fv(locations.viewport, 1, viewport);
  }

  if (locations.color_matrix != -1) {
    float matrix[16];
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j)
        matrix[i * 4 + j] = SkScalarToFloat(params->color_matrix[j * 5 + i]);
    }
    gl_->UniformMatrix4fv(locations.color_matrix, 1, false, matrix);
  }
  static const float kScale = 1.0f / 255.0f;
  if (locations.color_offset != -1) {
    float offset[4];
    for (int i = 0; i < 4; ++i)
      offset[i] = SkScalarToFloat(params->color_matrix[i * 5 + 4]) * kScale;

    gl_->Uniform4fv(locations.color_offset, 1, offset);
  }

  if (locations.backdrop != -1) {
    DCHECK(params->background_texture || params->background_image_id);
    DCHECK_NE(locations.backdrop, 0);
    DCHECK_NE(locations.backdrop_rect, 0);

    gl_->Uniform1i(locations.backdrop, ++last_texture_unit);

    gl_->Uniform4f(locations.backdrop_rect, params->background_rect.x(),
                   params->background_rect.y(), params->background_rect.width(),
                   params->background_rect.height());

    if (params->background_image_id) {
      gl_->ActiveTexture(GL_TEXTURE0 + last_texture_unit);
      gl_->BindTexture(GL_TEXTURE_2D, params->background_image_id);
      gl_->ActiveTexture(GL_TEXTURE0);
      if (params->mask_for_background)
        gl_->Uniform1i(locations.original_backdrop, ++last_texture_unit);
    }
    if (params->background_texture) {
      params->shader_background_sampler_lock =
          base::WrapUnique(new ResourceProvider::ScopedSamplerGL(
              resource_provider_, params->background_texture->id(),
              GL_TEXTURE0 + last_texture_unit, GL_LINEAR));
      DCHECK_EQ(static_cast<GLenum>(GL_TEXTURE_2D),
                params->shader_background_sampler_lock->target());
    }
  }

  SetShaderOpacity(params->quad->shared_quad_state->opacity, locations.alpha);
  SetShaderQuadF(params->surface_quad, locations.quad);
}

void GLRenderer::DrawRPDQ(const DrawRenderPassDrawQuadParams& params) {
  DrawQuadGeometry(params.projection_matrix,
                   params.quad->shared_quad_state->quad_to_target_transform,
                   params.dst_rect, params.locations.matrix);

  // Flush the compositor context before the filter bitmap goes out of
  // scope, so the draw gets processed before the filter texture gets deleted.
  if (params.filter_image)
    gl_->Flush();

  if (!params.use_shaders_for_blending)
    RestoreBlendFuncToDefault(params.quad->shared_quad_state->blend_mode);
}

struct SolidColorProgramUniforms {
  unsigned program;
  unsigned matrix_location;
  unsigned viewport_location;
  unsigned quad_location;
  unsigned edge_location;
  unsigned color_location;
};

template <class T>
static void SolidColorUniformLocation(T program,
                                      SolidColorProgramUniforms* uniforms) {
  uniforms->program = program->program();
  uniforms->matrix_location = program->vertex_shader().matrix_location();
  uniforms->viewport_location = program->vertex_shader().viewport_location();
  uniforms->quad_location = program->vertex_shader().quad_location();
  uniforms->edge_location = program->vertex_shader().edge_location();
  uniforms->color_location = program->fragment_shader().color_location();
}

namespace {
// These functions determine if a quad, clipped by a clip_region contains
// the entire {top|bottom|left|right} edge.
bool is_top(const gfx::QuadF* clip_region, const DrawQuad* quad) {
  if (!quad->IsTopEdge())
    return false;
  if (!clip_region)
    return true;

  return std::abs(clip_region->p1().y()) < kAntiAliasingEpsilon &&
         std::abs(clip_region->p2().y()) < kAntiAliasingEpsilon;
}

bool is_bottom(const gfx::QuadF* clip_region, const DrawQuad* quad) {
  if (!quad->IsBottomEdge())
    return false;
  if (!clip_region)
    return true;

  return std::abs(clip_region->p3().y() -
                  quad->shared_quad_state->quad_layer_bounds.height()) <
             kAntiAliasingEpsilon &&
         std::abs(clip_region->p4().y() -
                  quad->shared_quad_state->quad_layer_bounds.height()) <
             kAntiAliasingEpsilon;
}

bool is_left(const gfx::QuadF* clip_region, const DrawQuad* quad) {
  if (!quad->IsLeftEdge())
    return false;
  if (!clip_region)
    return true;

  return std::abs(clip_region->p1().x()) < kAntiAliasingEpsilon &&
         std::abs(clip_region->p4().x()) < kAntiAliasingEpsilon;
}

bool is_right(const gfx::QuadF* clip_region, const DrawQuad* quad) {
  if (!quad->IsRightEdge())
    return false;
  if (!clip_region)
    return true;

  return std::abs(clip_region->p2().x() -
                  quad->shared_quad_state->quad_layer_bounds.width()) <
             kAntiAliasingEpsilon &&
         std::abs(clip_region->p3().x() -
                  quad->shared_quad_state->quad_layer_bounds.width()) <
             kAntiAliasingEpsilon;
}
}  // anonymous namespace

static gfx::QuadF GetDeviceQuadWithAntialiasingOnExteriorEdges(
    const LayerQuad& device_layer_edges,
    const gfx::Transform& device_transform,
    const gfx::QuadF& tile_quad,
    const gfx::QuadF* clip_region,
    const DrawQuad* quad) {
  auto tile_rect = gfx::RectF(quad->visible_rect);

  gfx::PointF bottom_right = tile_quad.p3();
  gfx::PointF bottom_left = tile_quad.p4();
  gfx::PointF top_left = tile_quad.p1();
  gfx::PointF top_right = tile_quad.p2();
  bool clipped = false;

  // Map points to device space. We ignore |clipped|, since the result of
  // |MapPoint()| still produces a valid point to draw the quad with. When
  // clipped, the point will be outside of the viewport. See crbug.com/416367.
  bottom_right = MathUtil::MapPoint(device_transform, bottom_right, &clipped);
  bottom_left = MathUtil::MapPoint(device_transform, bottom_left, &clipped);
  top_left = MathUtil::MapPoint(device_transform, top_left, &clipped);
  top_right = MathUtil::MapPoint(device_transform, top_right, &clipped);

  LayerQuad::Edge bottom_edge(bottom_right, bottom_left);
  LayerQuad::Edge left_edge(bottom_left, top_left);
  LayerQuad::Edge top_edge(top_left, top_right);
  LayerQuad::Edge right_edge(top_right, bottom_right);

  // Only apply anti-aliasing to edges not clipped by culling or scissoring.
  // If an edge is degenerate we do not want to replace it with a "proper" edge
  // as that will cause the quad to possibly expand is strange ways.
  if (!top_edge.degenerate() && is_top(clip_region, quad) &&
      tile_rect.y() == quad->rect.y()) {
    top_edge = device_layer_edges.top();
  }
  if (!left_edge.degenerate() && is_left(clip_region, quad) &&
      tile_rect.x() == quad->rect.x()) {
    left_edge = device_layer_edges.left();
  }
  if (!right_edge.degenerate() && is_right(clip_region, quad) &&
      tile_rect.right() == quad->rect.right()) {
    right_edge = device_layer_edges.right();
  }
  if (!bottom_edge.degenerate() && is_bottom(clip_region, quad) &&
      tile_rect.bottom() == quad->rect.bottom()) {
    bottom_edge = device_layer_edges.bottom();
  }

  float sign = tile_quad.IsCounterClockwise() ? -1 : 1;
  bottom_edge.scale(sign);
  left_edge.scale(sign);
  top_edge.scale(sign);
  right_edge.scale(sign);

  // Create device space quad.
  return LayerQuad(left_edge, top_edge, right_edge, bottom_edge).ToQuadF();
}

float GetTotalQuadError(const gfx::QuadF* clipped_quad,
                        const gfx::QuadF* ideal_rect) {
  return (clipped_quad->p1() - ideal_rect->p1()).LengthSquared() +
         (clipped_quad->p2() - ideal_rect->p2()).LengthSquared() +
         (clipped_quad->p3() - ideal_rect->p3()).LengthSquared() +
         (clipped_quad->p4() - ideal_rect->p4()).LengthSquared();
}

// Attempt to rotate the clipped quad until it lines up the most
// correctly. This is necessary because we check the edges of this
// quad against the expected left/right/top/bottom for anti-aliasing.
void AlignQuadToBoundingBox(gfx::QuadF* clipped_quad) {
  auto bounding_quad = gfx::QuadF(clipped_quad->BoundingBox());
  gfx::QuadF best_rotation = *clipped_quad;
  float least_error_amount = GetTotalQuadError(clipped_quad, &bounding_quad);
  for (size_t i = 1; i < 4; ++i) {
    clipped_quad->Realign(1);
    float new_error = GetTotalQuadError(clipped_quad, &bounding_quad);
    if (new_error < least_error_amount) {
      least_error_amount = new_error;
      best_rotation = *clipped_quad;
    }
  }
  *clipped_quad = best_rotation;
}

void InflateAntiAliasingDistances(const gfx::QuadF& quad,
                                  LayerQuad* device_layer_edges,
                                  float edge[24]) {
  DCHECK(!quad.BoundingBox().IsEmpty());
  LayerQuad device_layer_bounds(gfx::QuadF(quad.BoundingBox()));

  device_layer_edges->InflateAntiAliasingDistance();
  device_layer_edges->ToFloatArray(edge);

  device_layer_bounds.InflateAntiAliasingDistance();
  device_layer_bounds.ToFloatArray(&edge[12]);
}

// static
bool GLRenderer::ShouldAntialiasQuad(const gfx::QuadF& device_layer_quad,
                                     bool clipped,
                                     bool force_aa) {
  // AAing clipped quads is not supported by the code yet.
  if (clipped)
    return false;
  if (device_layer_quad.BoundingBox().IsEmpty())
    return false;
  if (force_aa)
    return true;

  bool is_axis_aligned_in_target = device_layer_quad.IsRectilinear();
  bool is_nearest_rect_within_epsilon =
      is_axis_aligned_in_target &&
      gfx::IsNearestRectWithinDistance(device_layer_quad.BoundingBox(),
                                       kAntiAliasingEpsilon);
  return !is_nearest_rect_within_epsilon;
}

// static
void GLRenderer::SetupQuadForClippingAndAntialiasing(
    const gfx::Transform& device_transform,
    const DrawQuad* quad,
    const gfx::QuadF* aa_quad,
    const gfx::QuadF* clip_region,
    gfx::QuadF* local_quad,
    float edge[24]) {
  gfx::QuadF rotated_clip;
  const gfx::QuadF* local_clip_region = clip_region;
  if (local_clip_region) {
    rotated_clip = *clip_region;
    AlignQuadToBoundingBox(&rotated_clip);
    local_clip_region = &rotated_clip;
  }

  if (!aa_quad) {
    if (local_clip_region)
      *local_quad = *local_clip_region;
    return;
  }

  LayerQuad device_layer_edges(*aa_quad);
  InflateAntiAliasingDistances(*aa_quad, &device_layer_edges, edge);

  // If we have a clip region then we are split, and therefore
  // by necessity, at least one of our edges is not an external
  // one.
  bool is_full_rect = quad->visible_rect == quad->rect;

  bool region_contains_all_outside_edges =
      is_full_rect &&
      (is_top(local_clip_region, quad) && is_left(local_clip_region, quad) &&
       is_bottom(local_clip_region, quad) && is_right(local_clip_region, quad));

  bool use_aa_on_all_four_edges =
      !local_clip_region && region_contains_all_outside_edges;

  gfx::QuadF device_quad;
  if (use_aa_on_all_four_edges) {
    device_quad = device_layer_edges.ToQuadF();
  } else {
    gfx::QuadF tile_quad(local_clip_region
                             ? *local_clip_region
                             : gfx::QuadF(gfx::RectF(quad->visible_rect)));
    device_quad = GetDeviceQuadWithAntialiasingOnExteriorEdges(
        device_layer_edges, device_transform, tile_quad, local_clip_region,
        quad);
  }

  *local_quad = MapQuadToLocalSpace(device_transform, device_quad);
}

// static
void GLRenderer::SetupRenderPassQuadForClippingAndAntialiasing(
    const gfx::Transform& device_transform,
    const RenderPassDrawQuad* quad,
    const gfx::QuadF* aa_quad,
    const gfx::QuadF* clip_region,
    gfx::QuadF* local_quad,
    float edge[24]) {
  gfx::QuadF rotated_clip;
  const gfx::QuadF* local_clip_region = clip_region;
  if (local_clip_region) {
    rotated_clip = *clip_region;
    AlignQuadToBoundingBox(&rotated_clip);
    local_clip_region = &rotated_clip;
  }

  if (!aa_quad) {
    GetScaledRegion(quad->rect, local_clip_region, local_quad);
    return;
  }

  LayerQuad device_layer_edges(*aa_quad);
  InflateAntiAliasingDistances(*aa_quad, &device_layer_edges, edge);

  gfx::QuadF device_quad;

  // Apply anti-aliasing only to the edges that are not being clipped
  if (local_clip_region) {
    gfx::QuadF tile_quad(gfx::RectF(quad->visible_rect));
    GetScaledRegion(quad->rect, local_clip_region, &tile_quad);
    device_quad = GetDeviceQuadWithAntialiasingOnExteriorEdges(
        device_layer_edges, device_transform, tile_quad, local_clip_region,
        quad);
  } else {
    device_quad = device_layer_edges.ToQuadF();
  }

  *local_quad = MapQuadToLocalSpace(device_transform, device_quad);
}

void GLRenderer::DrawSolidColorQuad(const DrawingFrame* frame,
                                    const SolidColorDrawQuad* quad,
                                    const gfx::QuadF* clip_region) {
  gfx::Rect tile_rect = quad->visible_rect;

  SkColor color = quad->color;
  float opacity = quad->shared_quad_state->opacity;
  float alpha = (SkColorGetA(color) * (1.0f / 255.0f)) * opacity;

  // Early out if alpha is small enough that quad doesn't contribute to output.
  if (alpha < std::numeric_limits<float>::epsilon() &&
      quad->ShouldDrawWithBlending())
    return;

  gfx::Transform device_transform =
      frame->window_matrix * frame->projection_matrix *
      quad->shared_quad_state->quad_to_target_transform;
  device_transform.FlattenTo2d();
  if (!device_transform.IsInvertible())
    return;

  auto local_quad = gfx::QuadF(gfx::RectF(tile_rect));

  gfx::QuadF device_layer_quad;
  bool use_aa = false;
  bool allow_aa = settings_->allow_antialiasing &&
                  !quad->force_anti_aliasing_off && quad->IsEdge();

  if (allow_aa) {
    bool clipped = false;
    bool force_aa = false;
    device_layer_quad = MathUtil::MapQuad(
        device_transform,
        gfx::QuadF(
            gfx::RectF(quad->shared_quad_state->visible_quad_layer_rect)),
        &clipped);
    use_aa = ShouldAntialiasQuad(device_layer_quad, clipped, force_aa);
  }

  float edge[24];
  const gfx::QuadF* aa_quad = use_aa ? &device_layer_quad : nullptr;
  SetupQuadForClippingAndAntialiasing(device_transform, quad, aa_quad,
                                      clip_region, &local_quad, edge);

  SolidColorProgramUniforms uniforms;
  if (use_aa) {
    SolidColorUniformLocation(GetSolidColorProgramAA(), &uniforms);
  } else {
    SolidColorUniformLocation(GetSolidColorProgram(), &uniforms);
  }
  SetUseProgram(uniforms.program);

  gl_->Uniform4f(uniforms.color_location,
                 (SkColorGetR(color) * (1.0f / 255.0f)) * alpha,
                 (SkColorGetG(color) * (1.0f / 255.0f)) * alpha,
                 (SkColorGetB(color) * (1.0f / 255.0f)) * alpha, alpha);
  if (use_aa) {
    float viewport[4] = {
        static_cast<float>(current_window_space_viewport_.x()),
        static_cast<float>(current_window_space_viewport_.y()),
        static_cast<float>(current_window_space_viewport_.width()),
        static_cast<float>(current_window_space_viewport_.height()),
    };
    gl_->Uniform4fv(uniforms.viewport_location, 1, viewport);
    gl_->Uniform3fv(uniforms.edge_location, 8, edge);
  }

  // Enable blending when the quad properties require it or if we decided
  // to use antialiasing.
  SetBlendEnabled(quad->ShouldDrawWithBlending() || use_aa);

  // Antialising requires a normalized quad, but this could lead to floating
  // point precision errors, so only normalize when antialising is on.
  if (use_aa) {
    // Normalize to tile_rect.
    local_quad.Scale(1.0f / tile_rect.width(), 1.0f / tile_rect.height());

    SetShaderQuadF(local_quad, uniforms.quad_location);

    // The transform and vertex data are used to figure out the extents that the
    // un-antialiased quad should have and which vertex this is and the float
    // quad passed in via uniform is the actual geometry that gets used to draw
    // it. This is why this centered rect is used and not the original
    // quad_rect.
    gfx::RectF centered_rect(
        gfx::PointF(-0.5f * tile_rect.width(), -0.5f * tile_rect.height()),
        gfx::SizeF(tile_rect.size()));
    DrawQuadGeometry(frame->projection_matrix,
                     quad->shared_quad_state->quad_to_target_transform,
                     centered_rect, uniforms.matrix_location);
  } else {
    PrepareGeometry(SHARED_BINDING);
    SetShaderQuadF(local_quad, uniforms.quad_location);
    static float gl_matrix[16];
    ToGLMatrix(&gl_matrix[0],
               frame->projection_matrix *
                   quad->shared_quad_state->quad_to_target_transform);
    gl_->UniformMatrix4fv(uniforms.matrix_location, 1, false, &gl_matrix[0]);

    gl_->DrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
  }
}

struct TileProgramUniforms {
  unsigned program;
  unsigned matrix_location;
  unsigned viewport_location;
  unsigned quad_location;
  unsigned edge_location;
  unsigned vertex_tex_transform_location;
  unsigned sampler_location;
  unsigned fragment_tex_transform_location;
  unsigned alpha_location;
};

template <class T>
static void TileUniformLocation(T program, TileProgramUniforms* uniforms) {
  uniforms->program = program->program();
  uniforms->matrix_location = program->vertex_shader().matrix_location();
  uniforms->viewport_location = program->vertex_shader().viewport_location();
  uniforms->quad_location = program->vertex_shader().quad_location();
  uniforms->edge_location = program->vertex_shader().edge_location();
  uniforms->vertex_tex_transform_location =
      program->vertex_shader().vertex_tex_transform_location();

  uniforms->sampler_location = program->fragment_shader().sampler_location();
  uniforms->alpha_location = program->fragment_shader().alpha_location();
  uniforms->fragment_tex_transform_location =
      program->fragment_shader().fragment_tex_transform_location();
}

void GLRenderer::DrawTileQuad(const DrawingFrame* frame,
                              const TileDrawQuad* quad,
                              const gfx::QuadF* clip_region) {
  DrawContentQuad(frame, quad, quad->resource_id(), clip_region);
}

void GLRenderer::DrawContentQuad(const DrawingFrame* frame,
                                 const ContentDrawQuadBase* quad,
                                 ResourceId resource_id,
                                 const gfx::QuadF* clip_region) {
  gfx::Transform device_transform =
      frame->window_matrix * frame->projection_matrix *
      quad->shared_quad_state->quad_to_target_transform;
  device_transform.FlattenTo2d();

  gfx::QuadF device_layer_quad;
  bool use_aa = false;
  bool allow_aa = settings_->allow_antialiasing && quad->IsEdge();
  if (allow_aa) {
    bool clipped = false;
    bool force_aa = false;
    device_layer_quad = MathUtil::MapQuad(
        device_transform,
        gfx::QuadF(
            gfx::RectF(quad->shared_quad_state->visible_quad_layer_rect)),
        &clipped);
    use_aa = ShouldAntialiasQuad(device_layer_quad, clipped, force_aa);
  }

  // TODO(timav): simplify coordinate transformations in DrawContentQuadAA
  // similar to the way DrawContentQuadNoAA works and then consider
  // combining DrawContentQuadAA and DrawContentQuadNoAA into one method.
  if (use_aa)
    DrawContentQuadAA(frame, quad, resource_id, device_transform,
                      device_layer_quad, clip_region);
  else
    DrawContentQuadNoAA(frame, quad, resource_id, clip_region);
}

void GLRenderer::DrawContentQuadAA(const DrawingFrame* frame,
                                   const ContentDrawQuadBase* quad,
                                   ResourceId resource_id,
                                   const gfx::Transform& device_transform,
                                   const gfx::QuadF& aa_quad,
                                   const gfx::QuadF* clip_region) {
  if (!device_transform.IsInvertible())
    return;

  gfx::Rect tile_rect = quad->visible_rect;

  gfx::RectF tex_coord_rect = MathUtil::ScaleRectProportional(
      quad->tex_coord_rect, gfx::RectF(quad->rect), gfx::RectF(tile_rect));
  float tex_to_geom_scale_x = quad->rect.width() / quad->tex_coord_rect.width();
  float tex_to_geom_scale_y =
      quad->rect.height() / quad->tex_coord_rect.height();

  gfx::RectF clamp_geom_rect(tile_rect);
  gfx::RectF clamp_tex_rect(tex_coord_rect);
  // Clamp texture coordinates to avoid sampling outside the layer
  // by deflating the tile region half a texel or half a texel
  // minus epsilon for one pixel layers. The resulting clamp region
  // is mapped to the unit square by the vertex shader and mapped
  // back to normalized texture coordinates by the fragment shader
  // after being clamped to 0-1 range.
  float tex_clamp_x =
      std::min(0.5f, 0.5f * clamp_tex_rect.width() - kAntiAliasingEpsilon);
  float tex_clamp_y =
      std::min(0.5f, 0.5f * clamp_tex_rect.height() - kAntiAliasingEpsilon);
  float geom_clamp_x =
      std::min(tex_clamp_x * tex_to_geom_scale_x,
               0.5f * clamp_geom_rect.width() - kAntiAliasingEpsilon);
  float geom_clamp_y =
      std::min(tex_clamp_y * tex_to_geom_scale_y,
               0.5f * clamp_geom_rect.height() - kAntiAliasingEpsilon);
  clamp_geom_rect.Inset(geom_clamp_x, geom_clamp_y, geom_clamp_x, geom_clamp_y);
  clamp_tex_rect.Inset(tex_clamp_x, tex_clamp_y, tex_clamp_x, tex_clamp_y);

  // Map clamping rectangle to unit square.
  float vertex_tex_translate_x = -clamp_geom_rect.x() / clamp_geom_rect.width();
  float vertex_tex_translate_y =
      -clamp_geom_rect.y() / clamp_geom_rect.height();
  float vertex_tex_scale_x = tile_rect.width() / clamp_geom_rect.width();
  float vertex_tex_scale_y = tile_rect.height() / clamp_geom_rect.height();

  TexCoordPrecision tex_coord_precision = TexCoordPrecisionRequired(
      gl_, &highp_threshold_cache_, highp_threshold_min_, quad->texture_size);

  auto local_quad = gfx::QuadF(gfx::RectF(tile_rect));
  float edge[24];
  SetupQuadForClippingAndAntialiasing(device_transform, quad, &aa_quad,
                                      clip_region, &local_quad, edge);
  ResourceProvider::ScopedSamplerGL quad_resource_lock(
      resource_provider_, resource_id,
      quad->nearest_neighbor ? GL_NEAREST : GL_LINEAR);
  SamplerType sampler =
      SamplerTypeFromTextureTarget(quad_resource_lock.target());

  float fragment_tex_translate_x = clamp_tex_rect.x();
  float fragment_tex_translate_y = clamp_tex_rect.y();
  float fragment_tex_scale_x = clamp_tex_rect.width();
  float fragment_tex_scale_y = clamp_tex_rect.height();

  // Map to normalized texture coordinates.
  if (sampler != SAMPLER_TYPE_2D_RECT) {
    gfx::Size texture_size = quad->texture_size;
    DCHECK(!texture_size.IsEmpty());
    fragment_tex_translate_x /= texture_size.width();
    fragment_tex_translate_y /= texture_size.height();
    fragment_tex_scale_x /= texture_size.width();
    fragment_tex_scale_y /= texture_size.height();
  }

  TileProgramUniforms uniforms;
  if (quad->swizzle_contents) {
    TileUniformLocation(GetTileProgramSwizzleAA(tex_coord_precision, sampler),
                        &uniforms);
  } else {
    TileUniformLocation(GetTileProgramAA(tex_coord_precision, sampler),
                        &uniforms);
  }

  SetUseProgram(uniforms.program);
  gl_->Uniform1i(uniforms.sampler_location, 0);

  float viewport[4] = {
      static_cast<float>(current_window_space_viewport_.x()),
      static_cast<float>(current_window_space_viewport_.y()),
      static_cast<float>(current_window_space_viewport_.width()),
      static_cast<float>(current_window_space_viewport_.height()),
  };
  gl_->Uniform4fv(uniforms.viewport_location, 1, viewport);
  gl_->Uniform3fv(uniforms.edge_location, 8, edge);

  gl_->Uniform4f(uniforms.vertex_tex_transform_location, vertex_tex_translate_x,
                 vertex_tex_translate_y, vertex_tex_scale_x,
                 vertex_tex_scale_y);
  gl_->Uniform4f(uniforms.fragment_tex_transform_location,
                 fragment_tex_translate_x, fragment_tex_translate_y,
                 fragment_tex_scale_x, fragment_tex_scale_y);

  // Blending is required for antialiasing.
  SetBlendEnabled(true);

  // Normalize to tile_rect.
  local_quad.Scale(1.0f / tile_rect.width(), 1.0f / tile_rect.height());

  SetShaderOpacity(quad->shared_quad_state->opacity, uniforms.alpha_location);
  SetShaderQuadF(local_quad, uniforms.quad_location);

  // The transform and vertex data are used to figure out the extents that the
  // un-antialiased quad should have and which vertex this is and the float
  // quad passed in via uniform is the actual geometry that gets used to draw
  // it. This is why this centered rect is used and not the original quad_rect.
  gfx::RectF centered_rect(
      gfx::PointF(-0.5f * tile_rect.width(), -0.5f * tile_rect.height()),
      gfx::SizeF(tile_rect.size()));
  DrawQuadGeometry(frame->projection_matrix,
                   quad->shared_quad_state->quad_to_target_transform,
                   centered_rect, uniforms.matrix_location);
}

void GLRenderer::DrawContentQuadNoAA(const DrawingFrame* frame,
                                     const ContentDrawQuadBase* quad,
                                     ResourceId resource_id,
                                     const gfx::QuadF* clip_region) {
  gfx::RectF tex_coord_rect = MathUtil::ScaleRectProportional(
      quad->tex_coord_rect, gfx::RectF(quad->rect),
      gfx::RectF(quad->visible_rect));
  float tex_to_geom_scale_x = quad->rect.width() / quad->tex_coord_rect.width();
  float tex_to_geom_scale_y =
      quad->rect.height() / quad->tex_coord_rect.height();

  bool scaled = (tex_to_geom_scale_x != 1.f || tex_to_geom_scale_y != 1.f);
  GLenum filter = (scaled ||
                   !quad->shared_quad_state->quad_to_target_transform
                        .IsIdentityOrIntegerTranslation()) &&
                          !quad->nearest_neighbor
                      ? GL_LINEAR
                      : GL_NEAREST;

  ResourceProvider::ScopedSamplerGL quad_resource_lock(
      resource_provider_, resource_id, filter);
  SamplerType sampler =
      SamplerTypeFromTextureTarget(quad_resource_lock.target());

  float vertex_tex_translate_x = tex_coord_rect.x();
  float vertex_tex_translate_y = tex_coord_rect.y();
  float vertex_tex_scale_x = tex_coord_rect.width();
  float vertex_tex_scale_y = tex_coord_rect.height();

  // Map to normalized texture coordinates.
  if (sampler != SAMPLER_TYPE_2D_RECT) {
    gfx::Size texture_size = quad->texture_size;
    DCHECK(!texture_size.IsEmpty());
    vertex_tex_translate_x /= texture_size.width();
    vertex_tex_translate_y /= texture_size.height();
    vertex_tex_scale_x /= texture_size.width();
    vertex_tex_scale_y /= texture_size.height();
  }

  TexCoordPrecision tex_coord_precision = TexCoordPrecisionRequired(
      gl_, &highp_threshold_cache_, highp_threshold_min_, quad->texture_size);

  TileProgramUniforms uniforms;
  if (quad->ShouldDrawWithBlending()) {
    if (quad->swizzle_contents) {
      TileUniformLocation(GetTileProgramSwizzle(tex_coord_precision, sampler),
                          &uniforms);
    } else {
      TileUniformLocation(GetTileProgram(tex_coord_precision, sampler),
                          &uniforms);
    }
  } else {
    if (quad->swizzle_contents) {
      TileUniformLocation(
          GetTileProgramSwizzleOpaque(tex_coord_precision, sampler), &uniforms);
    } else {
      TileUniformLocation(GetTileProgramOpaque(tex_coord_precision, sampler),
                          &uniforms);
    }
  }

  SetUseProgram(uniforms.program);
  gl_->Uniform1i(uniforms.sampler_location, 0);

  gl_->Uniform4f(uniforms.vertex_tex_transform_location, vertex_tex_translate_x,
                 vertex_tex_translate_y, vertex_tex_scale_x,
                 vertex_tex_scale_y);

  SetBlendEnabled(quad->ShouldDrawWithBlending());

  SetShaderOpacity(quad->shared_quad_state->opacity, uniforms.alpha_location);

  // Pass quad coordinates to the uniform in the same order as GeometryBinding
  // does, then vertices will match the texture mapping in the vertex buffer.
  // The method SetShaderQuadF() changes the order of vertices and so it's
  // not used here.
  auto tile_quad = gfx::QuadF(gfx::RectF(quad->visible_rect));
  float width = quad->visible_rect.width();
  float height = quad->visible_rect.height();
  auto top_left = gfx::PointF(quad->visible_rect.origin());
  if (clip_region) {
    tile_quad = *clip_region;
    float gl_uv[8] = {
        (tile_quad.p4().x() - top_left.x()) / width,
        (tile_quad.p4().y() - top_left.y()) / height,
        (tile_quad.p1().x() - top_left.x()) / width,
        (tile_quad.p1().y() - top_left.y()) / height,
        (tile_quad.p2().x() - top_left.x()) / width,
        (tile_quad.p2().y() - top_left.y()) / height,
        (tile_quad.p3().x() - top_left.x()) / width,
        (tile_quad.p3().y() - top_left.y()) / height,
    };
    PrepareGeometry(CLIPPED_BINDING);
    clipped_geometry_->InitializeCustomQuadWithUVs(
        gfx::QuadF(gfx::RectF(quad->visible_rect)), gl_uv);
  } else {
    PrepareGeometry(SHARED_BINDING);
  }
  float gl_quad[8] = {
      tile_quad.p4().x(), tile_quad.p4().y(), tile_quad.p1().x(),
      tile_quad.p1().y(), tile_quad.p2().x(), tile_quad.p2().y(),
      tile_quad.p3().x(), tile_quad.p3().y(),
  };
  gl_->Uniform2fv(uniforms.quad_location, 4, gl_quad);

  static float gl_matrix[16];
  ToGLMatrix(&gl_matrix[0],
             frame->projection_matrix *
                 quad->shared_quad_state->quad_to_target_transform);
  gl_->UniformMatrix4fv(uniforms.matrix_location, 1, false, &gl_matrix[0]);

  gl_->DrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
}

void GLRenderer::DrawYUVVideoQuad(const DrawingFrame* frame,
                                  const YUVVideoDrawQuad* quad,
                                  const gfx::QuadF* clip_region) {
  SetBlendEnabled(quad->ShouldDrawWithBlending());

  TexCoordPrecision tex_coord_precision = TexCoordPrecisionRequired(
      gl_, &highp_threshold_cache_, highp_threshold_min_,
      quad->shared_quad_state->visible_quad_layer_rect.bottom_right());

  bool use_alpha_plane = quad->a_plane_resource_id() != 0;
  bool use_nv12 = quad->v_plane_resource_id() == quad->u_plane_resource_id();

  DCHECK(!(use_nv12 && use_alpha_plane));

  ResourceProvider::ScopedSamplerGL y_plane_lock(
      resource_provider_, quad->y_plane_resource_id(), GL_TEXTURE1, GL_LINEAR);
  ResourceProvider::ScopedSamplerGL u_plane_lock(
      resource_provider_, quad->u_plane_resource_id(), GL_TEXTURE2, GL_LINEAR);
  DCHECK_EQ(y_plane_lock.target(), u_plane_lock.target());
  // TODO(jbauman): Use base::Optional when available.
  std::unique_ptr<ResourceProvider::ScopedSamplerGL> v_plane_lock;
  if (!use_nv12) {
    v_plane_lock.reset(new ResourceProvider::ScopedSamplerGL(
        resource_provider_, quad->v_plane_resource_id(), GL_TEXTURE3,
        GL_LINEAR));
    DCHECK_EQ(y_plane_lock.target(), v_plane_lock->target());
  }
  std::unique_ptr<ResourceProvider::ScopedSamplerGL> a_plane_lock;
  if (use_alpha_plane) {
    a_plane_lock.reset(new ResourceProvider::ScopedSamplerGL(
        resource_provider_, quad->a_plane_resource_id(), GL_TEXTURE4,
        GL_LINEAR));
    DCHECK_EQ(y_plane_lock.target(), a_plane_lock->target());
  }

  // All planes must have the same sampler type.
  SamplerType sampler = SamplerTypeFromTextureTarget(y_plane_lock.target());

  int matrix_location = -1;
  int ya_tex_scale_location = -1;
  int ya_tex_offset_location = -1;
  int uv_tex_scale_location = -1;
  int uv_tex_offset_location = -1;
  int ya_clamp_rect_location = -1;
  int uv_clamp_rect_location = -1;
  int y_texture_location = -1;
  int u_texture_location = -1;
  int v_texture_location = -1;
  int uv_texture_location = -1;
  int a_texture_location = -1;
  int yuv_matrix_location = -1;
  int yuv_adj_location = -1;
  int alpha_location = -1;
  const VideoYUVProgram* program = GetVideoYUVProgram(
      tex_coord_precision, sampler, use_alpha_plane, use_nv12);
  DCHECK(program && (program->initialized() || IsContextLost()));
  SetUseProgram(program->program());
  matrix_location = program->vertex_shader().matrix_location();
  ya_tex_scale_location = program->vertex_shader().ya_tex_scale_location();
  ya_tex_offset_location = program->vertex_shader().ya_tex_offset_location();
  uv_tex_scale_location = program->vertex_shader().uv_tex_scale_location();
  uv_tex_offset_location = program->vertex_shader().uv_tex_offset_location();
  y_texture_location = program->fragment_shader().y_texture_location();
  u_texture_location = program->fragment_shader().u_texture_location();
  v_texture_location = program->fragment_shader().v_texture_location();
  uv_texture_location = program->fragment_shader().uv_texture_location();
  a_texture_location = program->fragment_shader().a_texture_location();
  yuv_matrix_location = program->fragment_shader().yuv_matrix_location();
  yuv_adj_location = program->fragment_shader().yuv_adj_location();
  ya_clamp_rect_location = program->fragment_shader().ya_clamp_rect_location();
  uv_clamp_rect_location = program->fragment_shader().uv_clamp_rect_location();
  alpha_location = program->fragment_shader().alpha_location();

  gfx::SizeF ya_tex_scale(1.0f, 1.0f);
  gfx::SizeF uv_tex_scale(1.0f, 1.0f);
  if (sampler != SAMPLER_TYPE_2D_RECT) {
    DCHECK(!quad->ya_tex_size.IsEmpty());
    DCHECK(!quad->uv_tex_size.IsEmpty());
    ya_tex_scale = gfx::SizeF(1.0f / quad->ya_tex_size.width(),
                              1.0f / quad->ya_tex_size.height());
    uv_tex_scale = gfx::SizeF(1.0f / quad->uv_tex_size.width(),
                              1.0f / quad->uv_tex_size.height());
  }

  float ya_vertex_tex_translate_x =
      quad->ya_tex_coord_rect.x() * ya_tex_scale.width();
  float ya_vertex_tex_translate_y =
      quad->ya_tex_coord_rect.y() * ya_tex_scale.height();
  float ya_vertex_tex_scale_x =
      quad->ya_tex_coord_rect.width() * ya_tex_scale.width();
  float ya_vertex_tex_scale_y =
      quad->ya_tex_coord_rect.height() * ya_tex_scale.height();

  float uv_vertex_tex_translate_x =
      quad->uv_tex_coord_rect.x() * uv_tex_scale.width();
  float uv_vertex_tex_translate_y =
      quad->uv_tex_coord_rect.y() * uv_tex_scale.height();
  float uv_vertex_tex_scale_x =
      quad->uv_tex_coord_rect.width() * uv_tex_scale.width();
  float uv_vertex_tex_scale_y =
      quad->uv_tex_coord_rect.height() * uv_tex_scale.height();

  gl_->Uniform2f(ya_tex_scale_location, ya_vertex_tex_scale_x,
                 ya_vertex_tex_scale_y);
  gl_->Uniform2f(ya_tex_offset_location, ya_vertex_tex_translate_x,
                 ya_vertex_tex_translate_y);
  gl_->Uniform2f(uv_tex_scale_location, uv_vertex_tex_scale_x,
                 uv_vertex_tex_scale_y);
  gl_->Uniform2f(uv_tex_offset_location, uv_vertex_tex_translate_x,
                 uv_vertex_tex_translate_y);

  gfx::RectF ya_clamp_rect(ya_vertex_tex_translate_x, ya_vertex_tex_translate_y,
                           ya_vertex_tex_scale_x, ya_vertex_tex_scale_y);
  ya_clamp_rect.Inset(0.5f * ya_tex_scale.width(),
                      0.5f * ya_tex_scale.height());
  gfx::RectF uv_clamp_rect(uv_vertex_tex_translate_x, uv_vertex_tex_translate_y,
                           uv_vertex_tex_scale_x, uv_vertex_tex_scale_y);
  uv_clamp_rect.Inset(0.5f * uv_tex_scale.width(),
                      0.5f * uv_tex_scale.height());
  gl_->Uniform4f(ya_clamp_rect_location, ya_clamp_rect.x(), ya_clamp_rect.y(),
                 ya_clamp_rect.right(), ya_clamp_rect.bottom());
  gl_->Uniform4f(uv_clamp_rect_location, uv_clamp_rect.x(), uv_clamp_rect.y(),
                 uv_clamp_rect.right(), uv_clamp_rect.bottom());

  gl_->Uniform1i(y_texture_location, 1);
  if (use_nv12) {
    gl_->Uniform1i(uv_texture_location, 2);
  } else {
    gl_->Uniform1i(u_texture_location, 2);
    gl_->Uniform1i(v_texture_location, 3);
  }
  if (use_alpha_plane)
    gl_->Uniform1i(a_texture_location, 4);

  // These values are magic numbers that are used in the transformation from YUV
  // to RGB color values.  They are taken from the following webpage:
  // http://www.fourcc.org/fccyvrgb.php
  float yuv_to_rgb_rec601[9] = {
      1.164f, 1.164f, 1.164f, 0.0f, -.391f, 2.018f, 1.596f, -.813f, 0.0f,
  };
  float yuv_to_rgb_jpeg[9] = {
      1.f, 1.f, 1.f, 0.0f, -.34414f, 1.772f, 1.402f, -.71414f, 0.0f,
  };
  float yuv_to_rgb_rec709[9] = {
      1.164f, 1.164f, 1.164f, 0.0f, -0.213f, 2.112f, 1.793f, -0.533f, 0.0f,
  };

  // They are used in the YUV to RGBA conversion formula:
  //   Y - 16   : Gives 16 values of head and footroom for overshooting
  //   U - 128  : Turns unsigned U into signed U [-128,127]
  //   V - 128  : Turns unsigned V into signed V [-128,127]
  float yuv_adjust_constrained[3] = {
      -16.f, -128.f, -128.f,
  };

  // Same as above, but without the head and footroom.
  float yuv_adjust_full[3] = {
      0.0f, -128.f, -128.f,
  };

  float* yuv_to_rgb = NULL;
  float* yuv_adjust = NULL;

  switch (quad->color_space) {
    case YUVVideoDrawQuad::REC_601:
      yuv_to_rgb = yuv_to_rgb_rec601;
      yuv_adjust = yuv_adjust_constrained;
      break;
    case YUVVideoDrawQuad::REC_709:
      yuv_to_rgb = yuv_to_rgb_rec709;
      yuv_adjust = yuv_adjust_constrained;
      break;
    case YUVVideoDrawQuad::JPEG:
      yuv_to_rgb = yuv_to_rgb_jpeg;
      yuv_adjust = yuv_adjust_full;
      break;
  }

  float yuv_to_rgb_multiplied[9];
  float yuv_adjust_with_offset[3];

  // Formula according to BT.601-7 section 2.5.3.
  DCHECK_LE(YUVVideoDrawQuad::kMinBitsPerChannel, quad->bits_per_channel);
  DCHECK_LE(quad->bits_per_channel, YUVVideoDrawQuad::kMaxBitsPerChannel);
  float adjustment_multiplier = (1 << (quad->bits_per_channel - 8)) * 1.0f /
                                ((1 << quad->bits_per_channel) - 1);

  for (int i = 0; i < 9; ++i)
    yuv_to_rgb_multiplied[i] = yuv_to_rgb[i] * quad->resource_multiplier;

  for (int i = 0; i < 3; ++i) {
    yuv_adjust_with_offset[i] =
        yuv_adjust[i] * adjustment_multiplier / quad->resource_multiplier -
        quad->resource_offset;
  }

  // The transform and vertex data are used to figure out the extents that the
  // un-antialiased quad should have and which vertex this is and the float
  // quad passed in via uniform is the actual geometry that gets used to draw
  // it. This is why this centered rect is used and not the original quad_rect.
  auto tile_rect = gfx::RectF(quad->rect);
  gl_->UniformMatrix3fv(yuv_matrix_location, 1, 0, yuv_to_rgb_multiplied);
  gl_->Uniform3fv(yuv_adj_location, 1, yuv_adjust_with_offset);

  SetShaderOpacity(quad->shared_quad_state->opacity, alpha_location);
  if (!clip_region) {
    DrawQuadGeometry(frame->projection_matrix,
                     quad->shared_quad_state->quad_to_target_transform,
                     tile_rect, matrix_location);
  } else {
    float uvs[8] = {0};
    GetScaledUVs(quad->visible_rect, clip_region, uvs);
    gfx::QuadF region_quad = *clip_region;
    region_quad.Scale(1.0f / tile_rect.width(), 1.0f / tile_rect.height());
    region_quad -= gfx::Vector2dF(0.5f, 0.5f);
    DrawQuadGeometryClippedByQuadF(
        frame, quad->shared_quad_state->quad_to_target_transform, tile_rect,
        region_quad, matrix_location, uvs);
  }
}

void GLRenderer::DrawStreamVideoQuad(const DrawingFrame* frame,
                                     const StreamVideoDrawQuad* quad,
                                     const gfx::QuadF* clip_region) {
  SetBlendEnabled(quad->ShouldDrawWithBlending());

  static float gl_matrix[16];

  DCHECK(capabilities_.using_egl_image);

  TexCoordPrecision tex_coord_precision = TexCoordPrecisionRequired(
      gl_, &highp_threshold_cache_, highp_threshold_min_,
      quad->shared_quad_state->visible_quad_layer_rect.bottom_right());

  const VideoStreamTextureProgram* program =
      GetVideoStreamTextureProgram(tex_coord_precision);
  SetUseProgram(program->program());

  ToGLMatrix(&gl_matrix[0], quad->matrix);

  ResourceProvider::ScopedReadLockGL lock(resource_provider_,
                                          quad->resource_id());

  DCHECK_EQ(GL_TEXTURE0, GetActiveTextureUnit(gl_));
  gl_->BindTexture(GL_TEXTURE_EXTERNAL_OES, lock.texture_id());

  gl_->UniformMatrix4fvStreamTextureMatrixCHROMIUM(
      program->vertex_shader().tex_matrix_location(), false, gl_matrix);

  gl_->Uniform1i(program->fragment_shader().sampler_location(), 0);

  SetShaderOpacity(quad->shared_quad_state->opacity,
                   program->fragment_shader().alpha_location());
  if (!clip_region) {
    DrawQuadGeometry(frame->projection_matrix,
                     quad->shared_quad_state->quad_to_target_transform,
                     gfx::RectF(quad->rect),
                     program->vertex_shader().matrix_location());
  } else {
    gfx::QuadF region_quad(*clip_region);
    region_quad.Scale(1.0f / quad->rect.width(), 1.0f / quad->rect.height());
    region_quad -= gfx::Vector2dF(0.5f, 0.5f);
    float uvs[8] = {0};
    GetScaledUVs(quad->visible_rect, clip_region, uvs);
    DrawQuadGeometryClippedByQuadF(
        frame, quad->shared_quad_state->quad_to_target_transform,
        gfx::RectF(quad->rect), region_quad,
        program->vertex_shader().matrix_location(), uvs);
  }
}

struct TextureProgramBinding {
  template <class Program>
  void Set(Program* program) {
    DCHECK(program);
    program_id = program->program();
    sampler_location = program->fragment_shader().sampler_location();
    matrix_location = program->vertex_shader().matrix_location();
    background_color_location =
        program->fragment_shader().background_color_location();
  }
  int program_id;
  int sampler_location;
  int matrix_location;
  int transform_location;
  int background_color_location;
};

struct TexTransformTextureProgramBinding : TextureProgramBinding {
  template <class Program>
  void Set(Program* program) {
    TextureProgramBinding::Set(program);
    tex_transform_location = program->vertex_shader().tex_transform_location();
    vertex_opacity_location =
        program->vertex_shader().vertex_opacity_location();
  }
  int tex_transform_location;
  int vertex_opacity_location;
};

void GLRenderer::FlushTextureQuadCache(BoundGeometry flush_binding) {
  // Check to see if we have anything to draw.
  if (draw_cache_.program_id == -1)
    return;

  PrepareGeometry(flush_binding);

  // Set the correct blending mode.
  SetBlendEnabled(draw_cache_.needs_blending);

  // Bind the program to the GL state.
  SetUseProgram(draw_cache_.program_id);

  // Bind the correct texture sampler location.
  gl_->Uniform1i(draw_cache_.sampler_location, 0);

  // Assume the current active textures is 0.
  ResourceProvider::ScopedSamplerGL locked_quad(
      resource_provider_,
      draw_cache_.resource_id,
      draw_cache_.nearest_neighbor ? GL_NEAREST : GL_LINEAR);
  DCHECK_EQ(GL_TEXTURE0, GetActiveTextureUnit(gl_));
  gl_->BindTexture(locked_quad.target(), locked_quad.texture_id());

  static_assert(sizeof(Float4) == 4 * sizeof(float),
                "Float4 struct should be densely packed");
  static_assert(sizeof(Float16) == 16 * sizeof(float),
                "Float16 struct should be densely packed");

  // Upload the tranforms for both points and uvs.
  gl_->UniformMatrix4fv(
      static_cast<int>(draw_cache_.matrix_location),
      static_cast<int>(draw_cache_.matrix_data.size()), false,
      reinterpret_cast<float*>(&draw_cache_.matrix_data.front()));
  gl_->Uniform4fv(static_cast<int>(draw_cache_.uv_xform_location),
                  static_cast<int>(draw_cache_.uv_xform_data.size()),
                  reinterpret_cast<float*>(&draw_cache_.uv_xform_data.front()));

  if (draw_cache_.background_color != SK_ColorTRANSPARENT) {
    Float4 background_color = PremultipliedColor(draw_cache_.background_color);
    gl_->Uniform4fv(draw_cache_.background_color_location, 1,
                    background_color.data);
  }

  gl_->Uniform1fv(
      static_cast<int>(draw_cache_.vertex_opacity_location),
      static_cast<int>(draw_cache_.vertex_opacity_data.size()),
      static_cast<float*>(&draw_cache_.vertex_opacity_data.front()));

  DCHECK_LE(draw_cache_.matrix_data.size(),
            static_cast<size_t>(std::numeric_limits<int>::max()) / 6u);
  // Draw the quads!
  gl_->DrawElements(GL_TRIANGLES,
                    6 * static_cast<int>(draw_cache_.matrix_data.size()),
                    GL_UNSIGNED_SHORT, 0);

  // Draw the border if requested.
  if (gl_composited_texture_quad_border_) {
    // When we draw the composited borders we have one flush per quad.
    DCHECK_EQ(1u, draw_cache_.matrix_data.size());
    SetBlendEnabled(false);
    const DebugBorderProgram* program = GetDebugBorderProgram();
    DCHECK(program && (program->initialized() || IsContextLost()));
    SetUseProgram(program->program());

    gl_->UniformMatrix4fv(
        program->vertex_shader().matrix_location(), 1, false,
        reinterpret_cast<float*>(&draw_cache_.matrix_data.front()));

    gl_->Uniform4f(program->fragment_shader().color_location(), 0.0f, 1.0f,
                   0.0f, 1.0f);

    gl_->LineWidth(3.0f);
    // The indices for the line are stored in the same array as the triangle
    // indices.
    gl_->DrawElements(GL_LINE_LOOP, 4, GL_UNSIGNED_SHORT, 0);
  }

  // Clear the cache.
  draw_cache_.program_id = -1;
  draw_cache_.uv_xform_data.resize(0);
  draw_cache_.vertex_opacity_data.resize(0);
  draw_cache_.matrix_data.resize(0);

  // If we had a clipped binding, prepare the shared binding for the
  // next inserts.
  if (flush_binding == CLIPPED_BINDING) {
    PrepareGeometry(SHARED_BINDING);
  }
}

void GLRenderer::EnqueueTextureQuad(const DrawingFrame* frame,
                                    const TextureDrawQuad* quad,
                                    const gfx::QuadF* clip_region) {
  // If we have a clip_region then we have to render the next quad
  // with dynamic geometry, therefore we must flush all pending
  // texture quads.
  if (clip_region) {
    // We send in false here because we want to flush what's currently in the
    // queue using the shared_geometry and not clipped_geometry
    FlushTextureQuadCache(SHARED_BINDING);
  }

  TexCoordPrecision tex_coord_precision = TexCoordPrecisionRequired(
      gl_, &highp_threshold_cache_, highp_threshold_min_,
      quad->shared_quad_state->visible_quad_layer_rect.bottom_right());

  ResourceProvider::ScopedReadLockGL lock(resource_provider_,
                                          quad->resource_id());
  const SamplerType sampler = SamplerTypeFromTextureTarget(lock.target());
  // Choose the correct texture program binding
  TexTransformTextureProgramBinding binding;
  if (quad->premultiplied_alpha) {
    if (quad->background_color == SK_ColorTRANSPARENT) {
      binding.Set(GetTextureProgram(tex_coord_precision, sampler));
    } else {
      binding.Set(GetTextureBackgroundProgram(tex_coord_precision, sampler));
    }
  } else {
    if (quad->background_color == SK_ColorTRANSPARENT) {
      binding.Set(
          GetNonPremultipliedTextureProgram(tex_coord_precision, sampler));
    } else {
      binding.Set(GetNonPremultipliedTextureBackgroundProgram(
          tex_coord_precision, sampler));
    }
  }

  int resource_id = quad->resource_id();

  size_t max_quads = StaticGeometryBinding::NUM_QUADS;
  if (draw_cache_.program_id != binding.program_id ||
      draw_cache_.resource_id != resource_id ||
      draw_cache_.needs_blending != quad->ShouldDrawWithBlending() ||
      draw_cache_.nearest_neighbor != quad->nearest_neighbor ||
      draw_cache_.background_color != quad->background_color ||
      draw_cache_.matrix_data.size() >= max_quads) {
    FlushTextureQuadCache(SHARED_BINDING);
    draw_cache_.program_id = binding.program_id;
    draw_cache_.resource_id = resource_id;
    draw_cache_.needs_blending = quad->ShouldDrawWithBlending();
    draw_cache_.nearest_neighbor = quad->nearest_neighbor;
    draw_cache_.background_color = quad->background_color;

    draw_cache_.uv_xform_location = binding.tex_transform_location;
    draw_cache_.background_color_location = binding.background_color_location;
    draw_cache_.vertex_opacity_location = binding.vertex_opacity_location;
    draw_cache_.matrix_location = binding.matrix_location;
    draw_cache_.sampler_location = binding.sampler_location;
  }

  // Generate the uv-transform
  Float4 uv_transform = {{0.0f, 0.0f, 1.0f, 1.0f}};
  if (!clip_region)
    uv_transform = UVTransform(quad);
  if (sampler == SAMPLER_TYPE_2D_RECT) {
    // Un-normalize the texture coordiantes for rectangle targets.
    gfx::Size texture_size = lock.size();
    uv_transform.data[0] *= texture_size.width();
    uv_transform.data[2] *= texture_size.width();
    uv_transform.data[1] *= texture_size.height();
    uv_transform.data[3] *= texture_size.height();
  }
  draw_cache_.uv_xform_data.push_back(uv_transform);

  // Generate the vertex opacity
  const float opacity = quad->shared_quad_state->opacity;
  draw_cache_.vertex_opacity_data.push_back(quad->vertex_opacity[0] * opacity);
  draw_cache_.vertex_opacity_data.push_back(quad->vertex_opacity[1] * opacity);
  draw_cache_.vertex_opacity_data.push_back(quad->vertex_opacity[2] * opacity);
  draw_cache_.vertex_opacity_data.push_back(quad->vertex_opacity[3] * opacity);

  // Generate the transform matrix
  gfx::Transform quad_rect_matrix;
  QuadRectTransform(&quad_rect_matrix,
                    quad->shared_quad_state->quad_to_target_transform,
                    gfx::RectF(quad->rect));
  quad_rect_matrix = frame->projection_matrix * quad_rect_matrix;

  Float16 m;
  quad_rect_matrix.matrix().asColMajorf(m.data);
  draw_cache_.matrix_data.push_back(m);

  if (clip_region) {
    gfx::QuadF scaled_region;
    if (!GetScaledRegion(quad->rect, clip_region, &scaled_region)) {
      scaled_region = SharedGeometryQuad().BoundingBox();
    }
    // Both the scaled region and the SharedGeomtryQuad are in the space
    // -0.5->0.5. We need to move that to the space 0->1.
    float uv[8];
    uv[0] = scaled_region.p1().x() + 0.5f;
    uv[1] = scaled_region.p1().y() + 0.5f;
    uv[2] = scaled_region.p2().x() + 0.5f;
    uv[3] = scaled_region.p2().y() + 0.5f;
    uv[4] = scaled_region.p3().x() + 0.5f;
    uv[5] = scaled_region.p3().y() + 0.5f;
    uv[6] = scaled_region.p4().x() + 0.5f;
    uv[7] = scaled_region.p4().y() + 0.5f;
    PrepareGeometry(CLIPPED_BINDING);
    clipped_geometry_->InitializeCustomQuadWithUVs(scaled_region, uv);
    FlushTextureQuadCache(CLIPPED_BINDING);
  } else if (gl_composited_texture_quad_border_) {
    FlushTextureQuadCache(SHARED_BINDING);
  }
}

void GLRenderer::FinishDrawingFrame(DrawingFrame* frame) {
  if (use_sync_query_) {
    DCHECK(current_sync_query_);
    current_sync_query_->End();
    pending_sync_queries_.push_back(std::move(current_sync_query_));
  }

  current_framebuffer_lock_ = nullptr;
  swap_buffer_rect_.Union(frame->root_damage_rect);

  gl_->Disable(GL_BLEND);
  blend_shadow_ = false;

  ScheduleCALayers(frame);
  ScheduleOverlays(frame);
}

void GLRenderer::FinishDrawingQuadList() {
  FlushTextureQuadCache(SHARED_BINDING);
}

bool GLRenderer::FlippedFramebuffer(const DrawingFrame* frame) const {
  if (frame->current_render_pass != frame->root_render_pass)
    return true;
  return FlippedRootFramebuffer();
}

bool GLRenderer::FlippedRootFramebuffer() const {
  // GL is normally flipped, so a flipped output results in an unflipping.
  return !output_surface_->capabilities().flipped_output_surface;
}

void GLRenderer::EnsureScissorTestEnabled() {
  if (is_scissor_enabled_)
    return;

  FlushTextureQuadCache(SHARED_BINDING);
  gl_->Enable(GL_SCISSOR_TEST);
  is_scissor_enabled_ = true;
}

void GLRenderer::EnsureScissorTestDisabled() {
  if (!is_scissor_enabled_)
    return;

  FlushTextureQuadCache(SHARED_BINDING);
  gl_->Disable(GL_SCISSOR_TEST);
  is_scissor_enabled_ = false;
}

void GLRenderer::CopyCurrentRenderPassToBitmap(
    DrawingFrame* frame,
    std::unique_ptr<CopyOutputRequest> request) {
  TRACE_EVENT0("cc", "GLRenderer::CopyCurrentRenderPassToBitmap");
  gfx::Rect copy_rect = frame->current_render_pass->output_rect;
  if (request->has_area())
    copy_rect.Intersect(request->area());
  GetFramebufferPixelsAsync(frame, copy_rect, std::move(request));
}

void GLRenderer::ToGLMatrix(float* gl_matrix, const gfx::Transform& transform) {
  transform.matrix().asColMajorf(gl_matrix);
}

void GLRenderer::SetShaderQuadF(const gfx::QuadF& quad, int quad_location) {
  if (quad_location == -1)
    return;

  float gl_quad[8];
  gl_quad[0] = quad.p1().x();
  gl_quad[1] = quad.p1().y();
  gl_quad[2] = quad.p2().x();
  gl_quad[3] = quad.p2().y();
  gl_quad[4] = quad.p3().x();
  gl_quad[5] = quad.p3().y();
  gl_quad[6] = quad.p4().x();
  gl_quad[7] = quad.p4().y();
  gl_->Uniform2fv(quad_location, 4, gl_quad);
}

void GLRenderer::SetShaderOpacity(float opacity, int alpha_location) {
  if (alpha_location != -1)
    gl_->Uniform1f(alpha_location, opacity);
}

void GLRenderer::SetStencilEnabled(bool enabled) {
  if (enabled == stencil_shadow_)
    return;

  if (enabled)
    gl_->Enable(GL_STENCIL_TEST);
  else
    gl_->Disable(GL_STENCIL_TEST);
  stencil_shadow_ = enabled;
}

void GLRenderer::SetBlendEnabled(bool enabled) {
  if (enabled == blend_shadow_)
    return;

  if (enabled)
    gl_->Enable(GL_BLEND);
  else
    gl_->Disable(GL_BLEND);
  blend_shadow_ = enabled;
}

void GLRenderer::SetUseProgram(unsigned program) {
  if (program == program_shadow_)
    return;
  gl_->UseProgram(program);
  program_shadow_ = program;
}

void GLRenderer::DrawQuadGeometryClippedByQuadF(
    const DrawingFrame* frame,
    const gfx::Transform& draw_transform,
    const gfx::RectF& quad_rect,
    const gfx::QuadF& clipping_region_quad,
    int matrix_location,
    const float* uvs) {
  PrepareGeometry(CLIPPED_BINDING);
  if (uvs) {
    clipped_geometry_->InitializeCustomQuadWithUVs(clipping_region_quad, uvs);
  } else {
    clipped_geometry_->InitializeCustomQuad(clipping_region_quad);
  }
  gfx::Transform quad_rect_matrix;
  QuadRectTransform(&quad_rect_matrix, draw_transform, quad_rect);
  static float gl_matrix[16];
  ToGLMatrix(&gl_matrix[0], frame->projection_matrix * quad_rect_matrix);
  gl_->UniformMatrix4fv(matrix_location, 1, false, &gl_matrix[0]);

  gl_->DrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT,
                    reinterpret_cast<const void*>(0));
}

void GLRenderer::DrawQuadGeometry(const gfx::Transform& projection_matrix,
                                  const gfx::Transform& draw_transform,
                                  const gfx::RectF& quad_rect,
                                  int matrix_location) {
  PrepareGeometry(SHARED_BINDING);
  gfx::Transform quad_rect_matrix;
  QuadRectTransform(&quad_rect_matrix, draw_transform, quad_rect);
  static float gl_matrix[16];
  ToGLMatrix(&gl_matrix[0], projection_matrix * quad_rect_matrix);
  gl_->UniformMatrix4fv(matrix_location, 1, false, &gl_matrix[0]);

  gl_->DrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
}

void GLRenderer::Finish() {
  TRACE_EVENT0("cc", "GLRenderer::Finish");
  gl_->Finish();
}

void GLRenderer::SwapBuffers(CompositorFrameMetadata metadata) {
  DCHECK(!is_backbuffer_discarded_);

  TRACE_EVENT0("cc,benchmark", "GLRenderer::SwapBuffers");
  // We're done! Time to swapbuffers!

  gfx::Size surface_size = output_surface_->SurfaceSize();

  CompositorFrame compositor_frame;
  compositor_frame.metadata = std::move(metadata);
  compositor_frame.gl_frame_data = base::WrapUnique(new GLFrameData);
  compositor_frame.gl_frame_data->size = surface_size;
  if (capabilities_.using_partial_swap) {
    // If supported, we can save significant bandwidth by only swapping the
    // damaged/scissored region (clamped to the viewport).
    swap_buffer_rect_.Intersect(gfx::Rect(surface_size));
    int flipped_y_pos_of_rect_bottom = surface_size.height() -
                                       swap_buffer_rect_.y() -
                                       swap_buffer_rect_.height();
    compositor_frame.gl_frame_data->sub_buffer_rect =
        gfx::Rect(swap_buffer_rect_.x(),
                  FlippedRootFramebuffer() ? flipped_y_pos_of_rect_bottom
                                           : swap_buffer_rect_.y(),
                  swap_buffer_rect_.width(), swap_buffer_rect_.height());
  } else {
    // Expand the swap rect to the full surface unless it's empty, and empty
    // swap is allowed.
    if (!swap_buffer_rect_.IsEmpty() || !capabilities_.allow_empty_swap) {
      swap_buffer_rect_ = gfx::Rect(surface_size);
    }
    compositor_frame.gl_frame_data->sub_buffer_rect = swap_buffer_rect_;
  }

  swapping_overlay_resources_.push_back(std::move(pending_overlay_resources_));
  pending_overlay_resources_.clear();

  // We always hold onto resources until an extra frame has swapped, to make
  // sure we don't update the buffer while it's being scanned out.
  if (!settings_->release_overlay_resources_after_gpu_query &&
      swapping_overlay_resources_.size() > 2) {
    swapping_overlay_resources_.pop_front();
  }

  output_surface_->SwapBuffers(std::move(compositor_frame));

  swap_buffer_rect_ = gfx::Rect();
}

void GLRenderer::SwapBuffersComplete() {
  if (settings_->release_overlay_resources_after_gpu_query) {
    // Once a resource has been swap-ACKed, send a query to the GPU process to
    // ask if the resource is no longer being consumed by the system compositor.
    // The response will come with the next swap-ACK.
    if (!swapping_overlay_resources_.empty()) {
      for (OverlayResourceLock& lock : swapping_overlay_resources_.front()) {
        unsigned texture = lock->texture_id();
        if (swapped_and_acked_overlay_resources_.find(texture) ==
            swapped_and_acked_overlay_resources_.end()) {
          swapped_and_acked_overlay_resources_[texture] = std::move(lock);
        }
      }
      swapping_overlay_resources_.pop_front();
    }

    if (!swapped_and_acked_overlay_resources_.empty()) {
      std::vector<unsigned> textures;
      textures.reserve(swapped_and_acked_overlay_resources_.size());
      for (auto& pair : swapped_and_acked_overlay_resources_) {
        textures.push_back(pair.first);
      }
      gl_->ScheduleCALayerInUseQueryCHROMIUM(textures.size(), textures.data());
    }
  } else if (swapping_overlay_resources_.size() > 1) {
    // If a query is not needed to release the overlay buffers, we can
    // assume that once a swap buffer is completed only the last set of
    // submitted overlay buffers are still in use by GL/Hardware Display.
    DCHECK_EQ(2u, swapping_overlay_resources_.size());
    swapping_overlay_resources_.pop_front();
  }
}

void GLRenderer::DidReceiveTextureInUseResponses(
    const gpu::TextureInUseResponses& responses) {
  DCHECK(settings_->release_overlay_resources_after_gpu_query);
  for (const gpu::TextureInUseResponse& response : responses) {
    if (!response.in_use) {
      swapped_and_acked_overlay_resources_.erase(response.texture);
    }
  }
}

void GLRenderer::EnforceMemoryPolicy() {
  if (!visible()) {
    TRACE_EVENT0("cc", "GLRenderer::EnforceMemoryPolicy dropping resources");
    ReleaseRenderPassTextures();
    DiscardBackbuffer();
    output_surface_->context_provider()->DeleteCachedResources();
    gl_->Flush();
  }
  PrepareGeometry(NO_BINDING);
}

void GLRenderer::DiscardBackbuffer() {
  if (is_backbuffer_discarded_)
    return;

  output_surface_->DiscardBackbuffer();

  is_backbuffer_discarded_ = true;

  // Damage tracker needs a full reset every time framebuffer is discarded.
  client_->SetFullRootLayerDamage();
}

void GLRenderer::EnsureBackbuffer() {
  if (!is_backbuffer_discarded_)
    return;

  output_surface_->EnsureBackbuffer();
  is_backbuffer_discarded_ = false;
}

void GLRenderer::GetFramebufferPixelsAsync(
    const DrawingFrame* frame,
    const gfx::Rect& rect,
    std::unique_ptr<CopyOutputRequest> request) {
  DCHECK(!request->IsEmpty());
  if (request->IsEmpty())
    return;
  if (rect.IsEmpty())
    return;

  gfx::Rect window_rect = MoveFromDrawToWindowSpace(frame, rect);
  DCHECK_GE(window_rect.x(), 0);
  DCHECK_GE(window_rect.y(), 0);
  DCHECK_LE(window_rect.right(), current_surface_size_.width());
  DCHECK_LE(window_rect.bottom(), current_surface_size_.height());

  if (!request->force_bitmap_result()) {
    bool own_mailbox = !request->has_texture_mailbox();

    GLuint texture_id = 0;
    gpu::Mailbox mailbox;
    if (own_mailbox) {
      gl_->GenMailboxCHROMIUM(mailbox.name);
      gl_->GenTextures(1, &texture_id);
      gl_->BindTexture(GL_TEXTURE_2D, texture_id);

      gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      gl_->ProduceTextureCHROMIUM(GL_TEXTURE_2D, mailbox.name);
    } else {
      mailbox = request->texture_mailbox().mailbox();
      DCHECK_EQ(static_cast<unsigned>(GL_TEXTURE_2D),
                request->texture_mailbox().target());
      DCHECK(!mailbox.IsZero());
      const gpu::SyncToken& incoming_sync_token =
          request->texture_mailbox().sync_token();
      if (incoming_sync_token.HasData())
        gl_->WaitSyncTokenCHROMIUM(incoming_sync_token.GetConstData());

      texture_id =
          gl_->CreateAndConsumeTextureCHROMIUM(GL_TEXTURE_2D, mailbox.name);
    }
    GetFramebufferTexture(texture_id, window_rect);

    const GLuint64 fence_sync = gl_->InsertFenceSyncCHROMIUM();
    gl_->ShallowFlushCHROMIUM();

    gpu::SyncToken sync_token;
    gl_->GenSyncTokenCHROMIUM(fence_sync, sync_token.GetData());

    TextureMailbox texture_mailbox(mailbox, sync_token, GL_TEXTURE_2D);

    std::unique_ptr<SingleReleaseCallback> release_callback;
    if (own_mailbox) {
      gl_->BindTexture(GL_TEXTURE_2D, 0);
      release_callback = texture_mailbox_deleter_->GetReleaseCallback(
          output_surface_->context_provider(), texture_id);
    } else {
      gl_->DeleteTextures(1, &texture_id);
    }

    request->SendTextureResult(window_rect.size(), texture_mailbox,
                               std::move(release_callback));
    return;
  }

  DCHECK(request->force_bitmap_result());

  std::unique_ptr<PendingAsyncReadPixels> pending_read(
      new PendingAsyncReadPixels);
  pending_read->copy_request = std::move(request);
  pending_async_read_pixels_.insert(pending_async_read_pixels_.begin(),
                                    std::move(pending_read));

  GLuint buffer = 0;
  gl_->GenBuffers(1, &buffer);
  gl_->BindBuffer(GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM, buffer);
  gl_->BufferData(GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM,
                  4 * window_rect.size().GetArea(), NULL, GL_STREAM_READ);

  GLuint query = 0;
  gl_->GenQueriesEXT(1, &query);
  gl_->BeginQueryEXT(GL_ASYNC_PIXEL_PACK_COMPLETED_CHROMIUM, query);

  gl_->ReadPixels(window_rect.x(), window_rect.y(), window_rect.width(),
                  window_rect.height(), GL_RGBA, GL_UNSIGNED_BYTE, NULL);

  gl_->BindBuffer(GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM, 0);

  base::Closure finished_callback = base::Bind(&GLRenderer::FinishedReadback,
                                               base::Unretained(this),
                                               buffer,
                                               query,
                                               window_rect.size());
  // Save the finished_callback so it can be cancelled.
  pending_async_read_pixels_.front()->finished_read_pixels_callback.Reset(
      finished_callback);
  base::Closure cancelable_callback =
      pending_async_read_pixels_.front()->
          finished_read_pixels_callback.callback();

  // Save the buffer to verify the callbacks happen in the expected order.
  pending_async_read_pixels_.front()->buffer = buffer;

  gl_->EndQueryEXT(GL_ASYNC_PIXEL_PACK_COMPLETED_CHROMIUM);
  context_support_->SignalQuery(query, cancelable_callback);

  EnforceMemoryPolicy();
}

void GLRenderer::FinishedReadback(unsigned source_buffer,
                                  unsigned query,
                                  const gfx::Size& size) {
  DCHECK(!pending_async_read_pixels_.empty());

  if (query != 0) {
    gl_->DeleteQueriesEXT(1, &query);
  }

  // Make sure we are servicing the right readback. There is no guarantee that
  // callbacks to this function are in the same order as we post the copy
  // requests.
  // Nevertheless, it is very likely that the order is preserved, and thus
  // start searching from back to the front.
  auto iter = pending_async_read_pixels_.rbegin();
  const auto& reverse_end = pending_async_read_pixels_.rend();
  while (iter != reverse_end && (*iter)->buffer != source_buffer)
    ++iter;

  DCHECK(iter != reverse_end);
  PendingAsyncReadPixels* current_read = iter->get();

  uint8_t* src_pixels = NULL;
  std::unique_ptr<SkBitmap> bitmap;

  if (source_buffer != 0) {
    gl_->BindBuffer(GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM, source_buffer);
    src_pixels = static_cast<uint8_t*>(gl_->MapBufferCHROMIUM(
        GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM, GL_READ_ONLY));

    if (src_pixels) {
      bitmap.reset(new SkBitmap);
      bitmap->allocN32Pixels(size.width(), size.height());
      std::unique_ptr<SkAutoLockPixels> lock(new SkAutoLockPixels(*bitmap));
      uint8_t* dest_pixels = static_cast<uint8_t*>(bitmap->getPixels());

      size_t row_bytes = size.width() * 4;
      int num_rows = size.height();
      size_t total_bytes = num_rows * row_bytes;
      for (size_t dest_y = 0; dest_y < total_bytes; dest_y += row_bytes) {
        // Flip Y axis.
        size_t src_y = total_bytes - dest_y - row_bytes;
        // Swizzle OpenGL -> Skia byte order.
        for (size_t x = 0; x < row_bytes; x += 4) {
          dest_pixels[dest_y + x + SK_R32_SHIFT / 8] =
              src_pixels[src_y + x + 0];
          dest_pixels[dest_y + x + SK_G32_SHIFT / 8] =
              src_pixels[src_y + x + 1];
          dest_pixels[dest_y + x + SK_B32_SHIFT / 8] =
              src_pixels[src_y + x + 2];
          dest_pixels[dest_y + x + SK_A32_SHIFT / 8] =
              src_pixels[src_y + x + 3];
        }
      }

      gl_->UnmapBufferCHROMIUM(GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM);
    }
    gl_->BindBuffer(GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM, 0);
    gl_->DeleteBuffers(1, &source_buffer);
  }

  if (bitmap)
    current_read->copy_request->SendBitmapResult(std::move(bitmap));

  // Conversion from reverse iterator to iterator:
  // Iterator |iter.base() - 1| points to the same element with reverse iterator
  // |iter|. The difference |-1| is due to the fact of correspondence of end()
  // with rbegin().
  pending_async_read_pixels_.erase(iter.base() - 1);
}

void GLRenderer::GetFramebufferTexture(unsigned texture_id,
                                       const gfx::Rect& window_rect) {
  DCHECK(texture_id);
  DCHECK_GE(window_rect.x(), 0);
  DCHECK_GE(window_rect.y(), 0);
  DCHECK_LE(window_rect.right(), current_surface_size_.width());
  DCHECK_LE(window_rect.bottom(), current_surface_size_.height());

  // If copying a non-root renderpass then use the format of the bound
  // texture. Otherwise, we use the format of the default framebuffer.
  GLenum format = current_framebuffer_lock_
                      ? GLCopyTextureInternalFormat(current_framebuffer_format_)
                      : output_surface_->GetFramebufferCopyTextureFormat();
  // Verify the format is valid for GLES2's glCopyTexImage2D.
  DCHECK(format == GL_ALPHA || format == GL_LUMINANCE ||
         format == GL_LUMINANCE_ALPHA || format == GL_RGB || format == GL_RGBA)
      << format;

  gl_->BindTexture(GL_TEXTURE_2D, texture_id);
  gl_->CopyTexImage2D(GL_TEXTURE_2D, 0, format, window_rect.x(),
                      window_rect.y(), window_rect.width(),
                      window_rect.height(), 0);
  gl_->BindTexture(GL_TEXTURE_2D, 0);
}

void GLRenderer::BindFramebufferToOutputSurface(DrawingFrame* frame) {
  current_framebuffer_lock_ = nullptr;
  output_surface_->BindFramebuffer();

  if (output_surface_->HasExternalStencilTest()) {
    output_surface_->ApplyExternalStencil();
    SetStencilEnabled(true);
  } else {
    SetStencilEnabled(false);
  }
}

bool GLRenderer::BindFramebufferToTexture(DrawingFrame* frame,
                                          const ScopedResource* texture) {
  DCHECK(texture->id());

  // Explicitly release lock, otherwise we can crash when try to lock
  // same texture again.
  current_framebuffer_lock_ = nullptr;

  SetStencilEnabled(false);
  gl_->BindFramebuffer(GL_FRAMEBUFFER, offscreen_framebuffer_id_);
  current_framebuffer_lock_ =
      base::WrapUnique(new ResourceProvider::ScopedWriteLockGL(
          resource_provider_, texture->id(), false));
  current_framebuffer_format_ = texture->format();
  unsigned texture_id = current_framebuffer_lock_->texture_id();
  gl_->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            texture_id, 0);

  DCHECK(gl_->CheckFramebufferStatus(GL_FRAMEBUFFER) ==
             GL_FRAMEBUFFER_COMPLETE ||
         IsContextLost());
  return true;
}

void GLRenderer::SetScissorTestRect(const gfx::Rect& scissor_rect) {
  EnsureScissorTestEnabled();

  // Don't unnecessarily ask the context to change the scissor, because it
  // may cause undesired GPU pipeline flushes.
  if (scissor_rect == scissor_rect_ && !scissor_rect_needs_reset_)
    return;

  scissor_rect_ = scissor_rect;
  FlushTextureQuadCache(SHARED_BINDING);
  gl_->Scissor(scissor_rect.x(), scissor_rect.y(), scissor_rect.width(),
               scissor_rect.height());

  scissor_rect_needs_reset_ = false;
}

void GLRenderer::SetViewport() {
  gl_->Viewport(current_window_space_viewport_.x(),
                current_window_space_viewport_.y(),
                current_window_space_viewport_.width(),
                current_window_space_viewport_.height());
}

void GLRenderer::InitializeSharedObjects() {
  TRACE_EVENT0("cc", "GLRenderer::InitializeSharedObjects");

  // Create an FBO for doing offscreen rendering.
  gl_->GenFramebuffers(1, &offscreen_framebuffer_id_);

  shared_geometry_ =
      base::WrapUnique(new StaticGeometryBinding(gl_, QuadVertexRect()));
  clipped_geometry_ = base::WrapUnique(new DynamicGeometryBinding(gl_));
}

void GLRenderer::PrepareGeometry(BoundGeometry binding) {
  if (binding == bound_geometry_) {
    return;
  }

  switch (binding) {
    case SHARED_BINDING:
      shared_geometry_->PrepareForDraw();
      break;
    case CLIPPED_BINDING:
      clipped_geometry_->PrepareForDraw();
      break;
    case NO_BINDING:
      break;
  }
  bound_geometry_ = binding;
}

const GLRenderer::DebugBorderProgram* GLRenderer::GetDebugBorderProgram() {
  if (!debug_border_program_.initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::debugBorderProgram::initialize");
    debug_border_program_.Initialize(output_surface_->context_provider(),
                                     TEX_COORD_PRECISION_NA, SAMPLER_TYPE_NA);
  }
  return &debug_border_program_;
}

const GLRenderer::SolidColorProgram* GLRenderer::GetSolidColorProgram() {
  if (!solid_color_program_.initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::solidColorProgram::initialize");
    solid_color_program_.Initialize(output_surface_->context_provider(),
                                    TEX_COORD_PRECISION_NA, SAMPLER_TYPE_NA);
  }
  return &solid_color_program_;
}

const GLRenderer::SolidColorProgramAA* GLRenderer::GetSolidColorProgramAA() {
  if (!solid_color_program_aa_.initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::solidColorProgramAA::initialize");
    solid_color_program_aa_.Initialize(output_surface_->context_provider(),
                                       TEX_COORD_PRECISION_NA, SAMPLER_TYPE_NA);
  }
  return &solid_color_program_aa_;
}

const GLRenderer::RenderPassProgram* GLRenderer::GetRenderPassProgram(
    TexCoordPrecision precision,
    BlendMode blend_mode) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LE(blend_mode, LAST_BLEND_MODE);
  RenderPassProgram* program = &render_pass_program_[precision][blend_mode];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::renderPassProgram::initialize");
    program->Initialize(output_surface_->context_provider(), precision,
                        SAMPLER_TYPE_2D, blend_mode);
  }
  return program;
}

const GLRenderer::RenderPassProgramAA* GLRenderer::GetRenderPassProgramAA(
    TexCoordPrecision precision,
    BlendMode blend_mode) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LE(blend_mode, LAST_BLEND_MODE);
  RenderPassProgramAA* program =
      &render_pass_program_aa_[precision][blend_mode];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::renderPassProgramAA::initialize");
    program->Initialize(output_surface_->context_provider(), precision,
                        SAMPLER_TYPE_2D, blend_mode);
  }
  return program;
}

const GLRenderer::RenderPassMaskProgram* GLRenderer::GetRenderPassMaskProgram(
    TexCoordPrecision precision,
    SamplerType sampler,
    BlendMode blend_mode,
    bool mask_for_background) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LE(blend_mode, LAST_BLEND_MODE);
  RenderPassMaskProgram* program =
      &render_pass_mask_program_[precision][sampler][blend_mode]
                                [mask_for_background ? HAS_MASK : NO_MASK];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::renderPassMaskProgram::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision,
        sampler, blend_mode, mask_for_background);
  }
  return program;
}

const GLRenderer::RenderPassMaskProgramAA*
GLRenderer::GetRenderPassMaskProgramAA(TexCoordPrecision precision,
                                       SamplerType sampler,
                                       BlendMode blend_mode,
                                       bool mask_for_background) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LE(blend_mode, LAST_BLEND_MODE);
  RenderPassMaskProgramAA* program =
      &render_pass_mask_program_aa_[precision][sampler][blend_mode]
                                   [mask_for_background ? HAS_MASK : NO_MASK];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::renderPassMaskProgramAA::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision,
        sampler, blend_mode, mask_for_background);
  }
  return program;
}

const GLRenderer::RenderPassColorMatrixProgram*
GLRenderer::GetRenderPassColorMatrixProgram(TexCoordPrecision precision,
                                            BlendMode blend_mode) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LE(blend_mode, LAST_BLEND_MODE);
  RenderPassColorMatrixProgram* program =
      &render_pass_color_matrix_program_[precision][blend_mode];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::renderPassColorMatrixProgram::initialize");
    program->Initialize(output_surface_->context_provider(), precision,
                        SAMPLER_TYPE_2D, blend_mode);
  }
  return program;
}

const GLRenderer::RenderPassColorMatrixProgramAA*
GLRenderer::GetRenderPassColorMatrixProgramAA(TexCoordPrecision precision,
                                              BlendMode blend_mode) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LE(blend_mode, LAST_BLEND_MODE);
  RenderPassColorMatrixProgramAA* program =
      &render_pass_color_matrix_program_aa_[precision][blend_mode];
  if (!program->initialized()) {
    TRACE_EVENT0("cc",
                 "GLRenderer::renderPassColorMatrixProgramAA::initialize");
    program->Initialize(output_surface_->context_provider(), precision,
                        SAMPLER_TYPE_2D, blend_mode);
  }
  return program;
}

const GLRenderer::RenderPassMaskColorMatrixProgram*
GLRenderer::GetRenderPassMaskColorMatrixProgram(
    TexCoordPrecision precision,
    SamplerType sampler,
    BlendMode blend_mode,
    bool mask_for_background) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LE(blend_mode, LAST_BLEND_MODE);
  RenderPassMaskColorMatrixProgram* program =
      &render_pass_mask_color_matrix_program_[precision][sampler][blend_mode]
          [mask_for_background ? HAS_MASK : NO_MASK];
  if (!program->initialized()) {
    TRACE_EVENT0("cc",
                 "GLRenderer::renderPassMaskColorMatrixProgram::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision,
        sampler, blend_mode, mask_for_background);
  }
  return program;
}

const GLRenderer::RenderPassMaskColorMatrixProgramAA*
GLRenderer::GetRenderPassMaskColorMatrixProgramAA(
    TexCoordPrecision precision,
    SamplerType sampler,
    BlendMode blend_mode,
    bool mask_for_background) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LE(blend_mode, LAST_BLEND_MODE);
  RenderPassMaskColorMatrixProgramAA* program =
      &render_pass_mask_color_matrix_program_aa_[precision][sampler][blend_mode]
          [mask_for_background ? HAS_MASK : NO_MASK];
  if (!program->initialized()) {
    TRACE_EVENT0("cc",
                 "GLRenderer::renderPassMaskColorMatrixProgramAA::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision,
        sampler, blend_mode, mask_for_background);
  }
  return program;
}

const GLRenderer::TileProgram* GLRenderer::GetTileProgram(
    TexCoordPrecision precision,
    SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  TileProgram* program = &tile_program_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::tileProgram::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler);
  }
  return program;
}

const GLRenderer::TileProgramOpaque* GLRenderer::GetTileProgramOpaque(
    TexCoordPrecision precision,
    SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  TileProgramOpaque* program = &tile_program_opaque_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::tileProgramOpaque::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler);
  }
  return program;
}

const GLRenderer::TileProgramAA* GLRenderer::GetTileProgramAA(
    TexCoordPrecision precision,
    SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  TileProgramAA* program = &tile_program_aa_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::tileProgramAA::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler);
  }
  return program;
}

const GLRenderer::TileProgramSwizzle* GLRenderer::GetTileProgramSwizzle(
    TexCoordPrecision precision,
    SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  TileProgramSwizzle* program = &tile_program_swizzle_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::tileProgramSwizzle::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler);
  }
  return program;
}

const GLRenderer::TileProgramSwizzleOpaque*
GLRenderer::GetTileProgramSwizzleOpaque(TexCoordPrecision precision,
                                        SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  TileProgramSwizzleOpaque* program =
      &tile_program_swizzle_opaque_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::tileProgramSwizzleOpaque::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler);
  }
  return program;
}

const GLRenderer::TileProgramSwizzleAA* GLRenderer::GetTileProgramSwizzleAA(
    TexCoordPrecision precision,
    SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  TileProgramSwizzleAA* program = &tile_program_swizzle_aa_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::tileProgramSwizzleAA::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler);
  }
  return program;
}

const GLRenderer::TextureProgram* GLRenderer::GetTextureProgram(
    TexCoordPrecision precision,
    SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  TextureProgram* program = &texture_program_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::textureProgram::initialize");
    program->Initialize(output_surface_->context_provider(), precision,
                        sampler);
  }
  return program;
}

const GLRenderer::NonPremultipliedTextureProgram*
GLRenderer::GetNonPremultipliedTextureProgram(TexCoordPrecision precision,
                                              SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  NonPremultipliedTextureProgram* program =
      &nonpremultiplied_texture_program_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc",
                 "GLRenderer::NonPremultipliedTextureProgram::Initialize");
    program->Initialize(output_surface_->context_provider(), precision,
                        sampler);
  }
  return program;
}

const GLRenderer::TextureBackgroundProgram*
GLRenderer::GetTextureBackgroundProgram(TexCoordPrecision precision,
                                        SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  TextureBackgroundProgram* program =
      &texture_background_program_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::textureProgram::initialize");
    program->Initialize(output_surface_->context_provider(), precision,
                        sampler);
  }
  return program;
}

const GLRenderer::NonPremultipliedTextureBackgroundProgram*
GLRenderer::GetNonPremultipliedTextureBackgroundProgram(
    TexCoordPrecision precision,
    SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  NonPremultipliedTextureBackgroundProgram* program =
      &nonpremultiplied_texture_background_program_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc",
                 "GLRenderer::NonPremultipliedTextureProgram::Initialize");
    program->Initialize(output_surface_->context_provider(), precision,
                        sampler);
  }
  return program;
}

const GLRenderer::VideoYUVProgram* GLRenderer::GetVideoYUVProgram(
    TexCoordPrecision precision,
    SamplerType sampler,
    bool use_alpha_plane,
    bool use_nv12) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  DCHECK_GE(sampler, 0);
  DCHECK_LE(sampler, LAST_SAMPLER_TYPE);
  VideoYUVProgram* program =
      &video_yuv_program_[precision][sampler][use_alpha_plane][use_nv12];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::videoYUVProgram::initialize");
    program->mutable_fragment_shader()->SetFeatures(use_alpha_plane, use_nv12);
    program->Initialize(output_surface_->context_provider(), precision,
                        sampler);
  }
  return program;
}

const GLRenderer::VideoStreamTextureProgram*
GLRenderer::GetVideoStreamTextureProgram(TexCoordPrecision precision) {
  DCHECK_GE(precision, 0);
  DCHECK_LE(precision, LAST_TEX_COORD_PRECISION);
  VideoStreamTextureProgram* program =
      &video_stream_texture_program_[precision];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::streamTextureProgram::initialize");
    program->Initialize(output_surface_->context_provider(), precision,
                        SAMPLER_TYPE_EXTERNAL_OES);
  }
  return program;
}

void GLRenderer::CleanupSharedObjects() {
  shared_geometry_ = nullptr;

  for (int i = 0; i <= LAST_TEX_COORD_PRECISION; ++i) {
    for (int j = 0; j <= LAST_SAMPLER_TYPE; ++j) {
      tile_program_[i][j].Cleanup(gl_);
      tile_program_opaque_[i][j].Cleanup(gl_);
      tile_program_swizzle_[i][j].Cleanup(gl_);
      tile_program_swizzle_opaque_[i][j].Cleanup(gl_);
      tile_program_aa_[i][j].Cleanup(gl_);
      tile_program_swizzle_aa_[i][j].Cleanup(gl_);

      for (int k = 0; k <= LAST_BLEND_MODE; k++) {
        for (int l = 0; l <= LAST_MASK_VALUE; ++l) {
          render_pass_mask_program_[i][j][k][l].Cleanup(gl_);
          render_pass_mask_program_aa_[i][j][k][l].Cleanup(gl_);
          render_pass_mask_color_matrix_program_aa_[i][j][k][l].Cleanup(gl_);
          render_pass_mask_color_matrix_program_[i][j][k][l].Cleanup(gl_);
        }
      }

      for (int k = 0; k < 2; k++) {
        for (int l = 0; l < 2; l++) {
          video_yuv_program_[i][j][k][l].Cleanup(gl_);
        }
      }
    }
    for (int j = 0; j <= LAST_BLEND_MODE; j++) {
      render_pass_program_[i][j].Cleanup(gl_);
      render_pass_program_aa_[i][j].Cleanup(gl_);
      render_pass_color_matrix_program_[i][j].Cleanup(gl_);
      render_pass_color_matrix_program_aa_[i][j].Cleanup(gl_);
    }

    for (int j = 0; j <= LAST_SAMPLER_TYPE; ++j) {
      texture_program_[i][j].Cleanup(gl_);
      nonpremultiplied_texture_program_[i][j].Cleanup(gl_);
      texture_background_program_[i][j].Cleanup(gl_);
      nonpremultiplied_texture_background_program_[i][j].Cleanup(gl_);
    }

    video_stream_texture_program_[i].Cleanup(gl_);
  }

  debug_border_program_.Cleanup(gl_);
  solid_color_program_.Cleanup(gl_);
  solid_color_program_aa_.Cleanup(gl_);

  if (offscreen_framebuffer_id_)
    gl_->DeleteFramebuffers(1, &offscreen_framebuffer_id_);

  ReleaseRenderPassTextures();
}

void GLRenderer::ReinitializeGLState() {
  is_scissor_enabled_ = false;
  scissor_rect_needs_reset_ = true;
  stencil_shadow_ = false;
  blend_shadow_ = true;
  program_shadow_ = 0;

  RestoreGLState();
}

void GLRenderer::RestoreGLState() {
  // This restores the current GLRenderer state to the GL context.
  bound_geometry_ = NO_BINDING;
  PrepareGeometry(SHARED_BINDING);

  gl_->Disable(GL_DEPTH_TEST);
  gl_->Disable(GL_CULL_FACE);
  gl_->ColorMask(true, true, true, true);
  gl_->BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  gl_->ActiveTexture(GL_TEXTURE0);

  if (program_shadow_)
    gl_->UseProgram(program_shadow_);

  if (stencil_shadow_)
    gl_->Enable(GL_STENCIL_TEST);
  else
    gl_->Disable(GL_STENCIL_TEST);

  if (blend_shadow_)
    gl_->Enable(GL_BLEND);
  else
    gl_->Disable(GL_BLEND);

  if (is_scissor_enabled_) {
    gl_->Enable(GL_SCISSOR_TEST);
    gl_->Scissor(scissor_rect_.x(), scissor_rect_.y(), scissor_rect_.width(),
                 scissor_rect_.height());
  } else {
    gl_->Disable(GL_SCISSOR_TEST);
  }
}

bool GLRenderer::IsContextLost() {
  return gl_->GetGraphicsResetStatusKHR() != GL_NO_ERROR;
}

void GLRenderer::ScheduleCALayers(DrawingFrame* frame) {
  scoped_refptr<CALayerOverlaySharedState> shared_state;
  size_t copied_render_pass_count = 0;
  for (const CALayerOverlay& ca_layer_overlay : frame->ca_layer_overlay_list) {
    if (!overlay_resource_pool_) {
      overlay_resource_pool_ = ResourcePool::CreateForGpuMemoryBufferResources(
          resource_provider_, base::ThreadTaskRunnerHandle::Get().get(),
          gfx::BufferUsage::SCANOUT);
    }

    ResourceId contents_resource_id = ca_layer_overlay.contents_resource_id;
    unsigned texture_id = 0;
    if (contents_resource_id) {
      pending_overlay_resources_.push_back(
          base::WrapUnique(new ResourceProvider::ScopedReadLockGL(
              resource_provider_, contents_resource_id)));
      texture_id = pending_overlay_resources_.back()->texture_id();
    }
    GLfloat contents_rect[4] = {
        ca_layer_overlay.contents_rect.x(), ca_layer_overlay.contents_rect.y(),
        ca_layer_overlay.contents_rect.width(),
        ca_layer_overlay.contents_rect.height(),
    };
    GLfloat bounds_rect[4] = {
        ca_layer_overlay.bounds_rect.x(), ca_layer_overlay.bounds_rect.y(),
        ca_layer_overlay.bounds_rect.width(),
        ca_layer_overlay.bounds_rect.height(),
    };
    GLboolean is_clipped = ca_layer_overlay.shared_state->is_clipped;
    GLfloat clip_rect[4] = {ca_layer_overlay.shared_state->clip_rect.x(),
                            ca_layer_overlay.shared_state->clip_rect.y(),
                            ca_layer_overlay.shared_state->clip_rect.width(),
                            ca_layer_overlay.shared_state->clip_rect.height()};
    GLint sorting_context_id =
        ca_layer_overlay.shared_state->sorting_context_id;
    GLfloat transform[16];
    ca_layer_overlay.shared_state->transform.asColMajorf(transform);
    unsigned filter = ca_layer_overlay.filter;

    if (ca_layer_overlay.shared_state != shared_state) {
      shared_state = ca_layer_overlay.shared_state;
      gl_->ScheduleCALayerSharedStateCHROMIUM(
          ca_layer_overlay.shared_state->opacity, is_clipped, clip_rect,
          sorting_context_id, transform);
    }
    gl_->ScheduleCALayerCHROMIUM(
        texture_id, contents_rect, ca_layer_overlay.background_color,
        ca_layer_overlay.edge_aa_mask, bounds_rect, filter);
  }

  // Take the number of copied render passes in this frame, and use 3 times that
  // amount as the cache limit.
  if (overlay_resource_pool_) {
    overlay_resource_pool_->SetResourceUsageLimits(
        std::numeric_limits<std::size_t>::max(), copied_render_pass_count * 3);
  }
}

void GLRenderer::ScheduleOverlays(DrawingFrame* frame) {
  if (frame->overlay_list.empty())
    return;

  OverlayCandidateList& overlays = frame->overlay_list;
  for (const OverlayCandidate& overlay : overlays) {
    unsigned texture_id = 0;
    if (overlay.use_output_surface_for_resource) {
      texture_id = output_surface_->GetOverlayTextureId();
      DCHECK(texture_id || IsContextLost());
    } else {
      pending_overlay_resources_.push_back(
          base::WrapUnique(new ResourceProvider::ScopedReadLockGL(
              resource_provider_, overlay.resource_id)));
      texture_id = pending_overlay_resources_.back()->texture_id();
    }

    context_support_->ScheduleOverlayPlane(
        overlay.plane_z_order, overlay.transform, texture_id,
        ToNearestRect(overlay.display_rect), overlay.uv_rect);
  }
}

}  // namespace cc

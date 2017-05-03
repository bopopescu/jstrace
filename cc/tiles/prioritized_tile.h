// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_TILES_PRIORITIZED_TILE_H_
#define CC_TILES_PRIORITIZED_TILE_H_

#include "cc/base/cc_export.h"
#include "cc/playback/raster_source.h"
#include "cc/tiles/picture_layer_tiling.h"
#include "cc/tiles/tile.h"
#include "cc/tiles/tile_priority.h"

namespace cc {
class RasterSource;
class PictureLayerTiling;

class CC_EXPORT PrioritizedTile {
 public:
  // This class is constructed and returned by a |PictureLayerTiling|, and
  // represents a tile and its priority.
  PrioritizedTile();
  PrioritizedTile(Tile* tile,
                  const PictureLayerTiling* source_tiling,
                  const TilePriority priority,
                  bool is_occluded);
  ~PrioritizedTile();

  Tile* tile() const { return tile_; }
  const scoped_refptr<RasterSource>& raster_source() const {
    return source_tiling_->raster_source();
  }
  const TilePriority& priority() const { return priority_; }
  bool is_occluded() const { return is_occluded_; }

  void AsValueInto(base::trace_event::TracedValue* value) const;

 private:
  Tile* tile_ = nullptr;
  const PictureLayerTiling* source_tiling_ = nullptr;
  TilePriority priority_;
  bool is_occluded_ = false;
};

}  // namespace cc

#endif  // CC_TILES_PRIORITIZED_TILE_H_

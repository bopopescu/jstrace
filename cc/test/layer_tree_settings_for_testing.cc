// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/test/layer_tree_settings_for_testing.h"

namespace cc {

LayerTreeSettingsForTesting::LayerTreeSettingsForTesting()
    : LayerTreeSettings() {
  verify_clip_tree_calculations = true;
  verify_transform_tree_calculations = true;
}

LayerTreeSettingsForTesting::~LayerTreeSettingsForTesting() {}

}  // namespace cc

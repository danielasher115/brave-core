// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/gestures/renderer/smart_gesture_content_extractor.h"

#include <memory>
#include <string_view>

#include "base/test/test_future.h"
#include "brave/components/gestures/core/common/mojom/smart_gesture_content_extractor.mojom.h"
#include "content/public/test/render_view_test.h"
#include "services/service_manager/public/cpp/binder_registry.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/public/web/web_view.h"
#include "ui/gfx/geometry/point_f.h"
#include "url/gurl.h"

namespace gestures {

class SmartGestureContentExtractorRenderViewTest
    : public content::RenderViewTest {
 public:
  SmartGestureContentExtractorRenderViewTest() = default;
  ~SmartGestureContentExtractorRenderViewTest() override = default;

  void SetUp() override {
    content::RenderViewTest::SetUp();
    // This harness has no real URL loader factory, so an `<img>`'s src
    // must never actually be fetched; the hit test only needs the URL
    // attribute, not a loaded image.
    web_view_->GetSettings()->SetImagesEnabled(false);
    binder_registry_ = std::make_unique<service_manager::BinderRegistry>();
    content_extractor_ = std::make_unique<SmartGestureContentExtractor>(
        GetMainRenderFrame(), binder_registry_.get());
  }

  void TearDown() override {
    content_extractor_.reset();
    binder_registry_.reset();
    content::RenderViewTest::TearDown();
  }

 protected:
  mojom::SmartGestureContentPtr GetContentAtHitTestPoint() {
    base::test::TestFuture<mojom::SmartGestureContentPtr> test_future;
    // Elements are positioned absolutely at the origin, so (10, 10) reliably
    // lands on whichever one the test's markup places there.
    content_extractor_->GetContentAtPoint(gfx::PointF(10, 10),
                                          test_future.GetCallback());
    return test_future.Take();
  }

 private:
  std::unique_ptr<service_manager::BinderRegistry> binder_registry_;
  std::unique_ptr<SmartGestureContentExtractor> content_extractor_;
};

TEST_F(SmartGestureContentExtractorRenderViewTest, ReturnsNullOverPlainText) {
  LoadHTML("<body>qux quux</body>");

  EXPECT_TRUE(GetContentAtHitTestPoint().is_null());
}

TEST_F(SmartGestureContentExtractorRenderViewTest, ReturnsLinkUrl) {
  LoadHTML(
      "<a href='https://foo.com/' "
      "style='position:absolute;left:0;top:0;width:50px;height:50px'>"
      "foo</a>");

  mojom::SmartGestureContentPtr gesture_content = GetContentAtHitTestPoint();

  ASSERT_TRUE(gesture_content->is_link_url());
  EXPECT_EQ(GURL("https://foo.com/"), gesture_content->get_link_url());
}

TEST_F(SmartGestureContentExtractorRenderViewTest, ReturnsImageUrl) {
  LoadHTML(
      "<img src='https://bar.com/baz.png' "
      "style='position:absolute;left:0;top:0;width:50px;height:50px'>");

  mojom::SmartGestureContentPtr gesture_content = GetContentAtHitTestPoint();

  ASSERT_TRUE(gesture_content->is_image_url());
  EXPECT_EQ(GURL("https://bar.com/baz.png"), gesture_content->get_image_url());
}

TEST_F(SmartGestureContentExtractorRenderViewTest,
       PrefersImageUrlOverOverlappingLinkUrl) {
  LoadHTML(
      "<a href='https://foo.com/' "
      "style='position:absolute;left:0;top:0;width:50px;height:50px'>"
      "<img src='https://bar.com/baz.png' "
      "style='position:absolute;left:0;top:0;width:50px;height:50px'>"
      "</a>");

  mojom::SmartGestureContentPtr gesture_content = GetContentAtHitTestPoint();

  ASSERT_TRUE(gesture_content->is_image_url());
  EXPECT_EQ(GURL("https://bar.com/baz.png"), gesture_content->get_image_url());
}

TEST_F(SmartGestureContentExtractorRenderViewTest,
       ReturnsImageUrlUnderAnUnrelatedOverlappingElement) {
  LoadHTML(
      "<img src='https://bar.com/baz.png' "
      "style='position:absolute;left:0;top:0;width:50px;height:50px'>"
      "<div style='position:absolute;left:0;top:0;width:50px;height:50px'>"
      "</div>");

  mojom::SmartGestureContentPtr gesture_content = GetContentAtHitTestPoint();

  ASSERT_TRUE(gesture_content->is_image_url());
  EXPECT_EQ(GURL("https://bar.com/baz.png"), gesture_content->get_image_url());
}

TEST_F(SmartGestureContentExtractorRenderViewTest,
       ReturnsSelectedTextWhenNoLinkOrImageIsAtThePoint) {
  LoadHTML("<body>qux quux</body>");
  ExecuteJavaScriptForTests("document.execCommand('selectAll')");

  mojom::SmartGestureContentPtr gesture_content = GetContentAtHitTestPoint();

  ASSERT_TRUE(gesture_content->is_selected_text());
  EXPECT_EQ("qux quux", gesture_content->get_selected_text());
}

}  // namespace gestures

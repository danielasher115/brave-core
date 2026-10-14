// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/browser/ui/gestures/input/gesture_input_registrar.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "brave/browser/ui/gestures/input/gesture_input_registrar_util.h"
#include "content/public/browser/render_widget_host_view.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"

namespace gestures {

GestureInputRegistrar::GestureInputRegistrar(
    content::RenderWidgetHost::MouseEventCallback mouse_event_callback,
    base::RepeatingCallback<void(const blink::WebInputEvent&,
                                 input::InputEventSource)> on_input_event)
    : mouse_event_callback_(std::move(mouse_event_callback)),
      on_input_event_(std::move(on_input_event)) {}

GestureInputRegistrar::~GestureInputRegistrar() {
  UnregisterAllWidgets();
}

void GestureInputRegistrar::UpdateObservedWidgets(
    content::WebContents* const web_contents) {
  const std::vector<content::RenderWidgetHost*> current_render_widget_hosts =
      CollectRenderWidgetHosts(web_contents);

  std::erase_if(
      observed_render_widget_hosts_,
      [&](const auto& observed_render_widget_host) {
        if (std::ranges::contains(current_render_widget_hosts,
                                  observed_render_widget_host.first)) {
          return false;
        }
        UnregisterFromWidget(*observed_render_widget_host.first);
        return true;
      });

  for (content::RenderWidgetHost* const render_widget_host :
       current_render_widget_hosts) {
    if (IsObservingWidget(*render_widget_host)) {
      continue;
    }
    content::RenderWidgetHost::MouseEventCallback widget_mouse_event_callback =
        base::BindRepeating(&GestureInputRegistrar::WidgetMouseEventCallback,
                            base::Unretained(this), render_widget_host);
    observed_render_widget_hosts_.emplace_back(render_widget_host,
                                               widget_mouse_event_callback);
    render_widget_host->AddMouseEventCallback(widget_mouse_event_callback);
    render_widget_host->AddInputEventObserver(this);
    render_widget_host->AddObserver(this);
  }
}

void GestureInputRegistrar::UnregisterWidget(
    content::RenderWidgetHost& render_widget_host) {
  if (!IsObservingWidget(render_widget_host)) {
    return;
  }
  UnregisterFromWidget(render_widget_host);
  std::erase_if(observed_render_widget_hosts_,
                [&](const auto& observed_render_widget_host) {
                  return observed_render_widget_host.first ==
                         &render_widget_host;
                });
}

void GestureInputRegistrar::UnregisterAllWidgets() {
  for (const auto& observed_render_widget_host :
       observed_render_widget_hosts_) {
    UnregisterFromWidget(*observed_render_widget_host.first);
  }
  observed_render_widget_hosts_.clear();
}

void GestureInputRegistrar::UnregisterFromWidget(
    content::RenderWidgetHost& render_widget_host) {
  const auto iter =
      std::ranges::find(observed_render_widget_hosts_, &render_widget_host,
                        [](const auto& observed_render_widget_host) {
                          return observed_render_widget_host.first.get();
                        });
  CHECK(iter != observed_render_widget_hosts_.cend());
  render_widget_host.RemoveMouseEventCallback(iter->second);
  render_widget_host.RemoveInputEventObserver(this);
  render_widget_host.RemoveObserver(this);
}

bool GestureInputRegistrar::IsObservingWidget(
    content::RenderWidgetHost& render_widget_host) const {
  return std::ranges::any_of(observed_render_widget_hosts_,
                             [&](const auto& observed_render_widget_host) {
                               return observed_render_widget_host.first ==
                                      &render_widget_host;
                             });
}

bool GestureInputRegistrar::WidgetMouseEventCallback(
    content::RenderWidgetHost* render_widget_host,
    const blink::WebMouseEvent& event) {
  content::RenderWidgetHostView* const view = render_widget_host->GetView();
  if (!view) {
    return mouse_event_callback_.Run(event);
  }
  // Coordinates from an out-of-process subframe's own widget (e.g. a PDF
  // viewer) are local to that widget, not the page's root coordinate space.
  blink::WebMouseEvent transformed_event = event;
  transformed_event.SetPositionInWidget(
      view->TransformPointToRootCoordSpaceF(event.PositionInWidget()));
  return mouse_event_callback_.Run(transformed_event);
}

void GestureInputRegistrar::OnInputEvent(
    const content::RenderWidgetHost& /*host*/,
    const blink::WebInputEvent& event,
    InputEventSource source) {
  on_input_event_.Run(event, source);
}

void GestureInputRegistrar::RenderWidgetHostDestroyed(
    content::RenderWidgetHost* widget_host) {
  // Called while `widget_host` is still valid, guaranteed to be the last
  // call made to this observer for it; stops tracking it before it's freed,
  // since `UpdateObservedWidgets`/`UnregisterAllWidgets` can't rely on a
  // later resync to notice it's already gone.
  UnregisterWidget(*widget_host);
}

}  // namespace gestures

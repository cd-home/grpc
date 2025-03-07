//
// Copyright 2018 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <grpc/support/port_platform.h>

#include <stddef.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"

#include <grpc/impl/codegen/connectivity_state.h>
#include <grpc/support/log.h>

#include "src/core/ext/filters/client_channel/client_channel.h"
#include "src/core/ext/filters/client_channel/lb_policy/child_policy_handler.h"
#include "src/core/ext/filters/client_channel/resolver/xds/xds_resolver.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/config/core_configuration.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/gprpp/debug_location.h"
#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/gprpp/ref_counted.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/gprpp/time.h"
#include "src/core/lib/gprpp/work_serializer.h"
#include "src/core/lib/iomgr/closure.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/pollset_set.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/json/json.h"
#include "src/core/lib/load_balancing/lb_policy.h"
#include "src/core/lib/load_balancing/lb_policy_factory.h"
#include "src/core/lib/load_balancing/lb_policy_registry.h"
#include "src/core/lib/load_balancing/subchannel_interface.h"
#include "src/core/lib/resolver/server_address.h"
#include "src/core/lib/transport/connectivity_state.h"

#define GRPC_XDS_CLUSTER_MANAGER_CHILD_RETENTION_INTERVAL_MS (15 * 60 * 1000)

namespace grpc_core {

TraceFlag grpc_xds_cluster_manager_lb_trace(false, "xds_cluster_manager_lb");

namespace {

constexpr absl::string_view kXdsClusterManager =
    "xds_cluster_manager_experimental";

// Config for xds_cluster_manager LB policy.
class XdsClusterManagerLbConfig : public LoadBalancingPolicy::Config {
 public:
  using ClusterMap =
      std::map<std::string, RefCountedPtr<LoadBalancingPolicy::Config>>;

  explicit XdsClusterManagerLbConfig(ClusterMap cluster_map)
      : cluster_map_(std::move(cluster_map)) {}

  absl::string_view name() const override { return kXdsClusterManager; }

  const ClusterMap& cluster_map() const { return cluster_map_; }

 private:
  ClusterMap cluster_map_;
};

// xds_cluster_manager LB policy.
class XdsClusterManagerLb : public LoadBalancingPolicy {
 public:
  explicit XdsClusterManagerLb(Args args);

  absl::string_view name() const override { return kXdsClusterManager; }

  void UpdateLocked(UpdateArgs args) override;
  void ExitIdleLocked() override;
  void ResetBackoffLocked() override;

 private:
  // A simple wrapper for ref-counting a picker from the child policy.
  class ChildPickerWrapper : public RefCounted<ChildPickerWrapper> {
   public:
    ChildPickerWrapper(std::string name,
                       std::unique_ptr<SubchannelPicker> picker)
        : name_(std::move(name)), picker_(std::move(picker)) {}
    PickResult Pick(PickArgs args) { return picker_->Pick(args); }

    const std::string& name() const { return name_; }

   private:
    std::string name_;
    std::unique_ptr<SubchannelPicker> picker_;
  };

  // Picks a child using prefix or path matching and then delegates to that
  // child's picker.
  class ClusterPicker : public SubchannelPicker {
   public:
    // Maintains a map of cluster names to pickers.
    using ClusterMap = std::map<absl::string_view /*cluster_name*/,
                                RefCountedPtr<ChildPickerWrapper>>;

    // It is required that the keys of cluster_map have to live at least as long
    // as the ClusterPicker instance.
    explicit ClusterPicker(ClusterMap cluster_map)
        : cluster_map_(std::move(cluster_map)) {}

    PickResult Pick(PickArgs args) override;

   private:
    ClusterMap cluster_map_;
  };

  // Each ClusterChild holds a ref to its parent XdsClusterManagerLb.
  class ClusterChild : public InternallyRefCounted<ClusterChild> {
   public:
    ClusterChild(RefCountedPtr<XdsClusterManagerLb> xds_cluster_manager_policy,
                 const std::string& name);
    ~ClusterChild() override;

    void Orphan() override;

    void UpdateLocked(RefCountedPtr<LoadBalancingPolicy::Config> config,
                      const absl::StatusOr<ServerAddressList>& addresses,
                      const ChannelArgs& args);
    void ExitIdleLocked();
    void ResetBackoffLocked();
    void DeactivateLocked();

    grpc_connectivity_state connectivity_state() const {
      return connectivity_state_;
    }
    RefCountedPtr<ChildPickerWrapper> picker_wrapper() const {
      return picker_wrapper_;
    }

   private:
    class Helper : public ChannelControlHelper {
     public:
      explicit Helper(RefCountedPtr<ClusterChild> xds_cluster_manager_child)
          : xds_cluster_manager_child_(std::move(xds_cluster_manager_child)) {}

      ~Helper() override {
        xds_cluster_manager_child_.reset(DEBUG_LOCATION, "Helper");
      }

      RefCountedPtr<SubchannelInterface> CreateSubchannel(
          ServerAddress address, const ChannelArgs& args) override;
      void UpdateState(grpc_connectivity_state state,
                       const absl::Status& status,
                       std::unique_ptr<SubchannelPicker> picker) override;
      void RequestReresolution() override;
      absl::string_view GetAuthority() override;
      void AddTraceEvent(TraceSeverity severity,
                         absl::string_view message) override;

     private:
      RefCountedPtr<ClusterChild> xds_cluster_manager_child_;
    };

    // Methods for dealing with the child policy.
    OrphanablePtr<LoadBalancingPolicy> CreateChildPolicyLocked(
        const ChannelArgs& args);

    static void OnDelayedRemovalTimer(void* arg, grpc_error_handle error);
    void OnDelayedRemovalTimerLocked(grpc_error_handle error);

    // The owning LB policy.
    RefCountedPtr<XdsClusterManagerLb> xds_cluster_manager_policy_;

    // Points to the corresponding key in children map.
    const std::string name_;

    OrphanablePtr<LoadBalancingPolicy> child_policy_;

    RefCountedPtr<ChildPickerWrapper> picker_wrapper_;
    grpc_connectivity_state connectivity_state_ = GRPC_CHANNEL_IDLE;

    // States for delayed removal.
    grpc_timer delayed_removal_timer_;
    grpc_closure on_delayed_removal_timer_;
    bool delayed_removal_timer_callback_pending_ = false;
    bool shutdown_ = false;
  };

  ~XdsClusterManagerLb() override;

  void ShutdownLocked() override;

  void UpdateStateLocked();

  // Current config from the resolver.
  RefCountedPtr<XdsClusterManagerLbConfig> config_;

  // Internal state.
  bool shutting_down_ = false;
  bool update_in_progress_ = false;

  // Children.
  std::map<std::string, OrphanablePtr<ClusterChild>> children_;
};

//
// XdsClusterManagerLb::ClusterPicker
//

XdsClusterManagerLb::PickResult XdsClusterManagerLb::ClusterPicker::Pick(
    PickArgs args) {
  auto* call_state = static_cast<ClientChannel::LoadBalancedCall::LbCallState*>(
      args.call_state);
  auto cluster_name =
      call_state->GetCallAttribute(XdsClusterAttributeTypeName());
  auto it = cluster_map_.find(cluster_name);
  if (it != cluster_map_.end()) {
    return it->second->Pick(args);
  }
  return PickResult::Fail(absl::InternalError(absl::StrCat(
      "xds cluster manager picker: unknown cluster \"", cluster_name, "\"")));
}

//
// XdsClusterManagerLb
//

XdsClusterManagerLb::XdsClusterManagerLb(Args args)
    : LoadBalancingPolicy(std::move(args)) {}

XdsClusterManagerLb::~XdsClusterManagerLb() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_cluster_manager_lb_trace)) {
    gpr_log(
        GPR_INFO,
        "[xds_cluster_manager_lb %p] destroying xds_cluster_manager LB policy",
        this);
  }
}

void XdsClusterManagerLb::ShutdownLocked() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_cluster_manager_lb_trace)) {
    gpr_log(GPR_INFO, "[xds_cluster_manager_lb %p] shutting down", this);
  }
  shutting_down_ = true;
  children_.clear();
}

void XdsClusterManagerLb::ExitIdleLocked() {
  for (auto& p : children_) p.second->ExitIdleLocked();
}

void XdsClusterManagerLb::ResetBackoffLocked() {
  for (auto& p : children_) p.second->ResetBackoffLocked();
}

void XdsClusterManagerLb::UpdateLocked(UpdateArgs args) {
  if (shutting_down_) return;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_cluster_manager_lb_trace)) {
    gpr_log(GPR_INFO, "[xds_cluster_manager_lb %p] Received update", this);
  }
  update_in_progress_ = true;
  // Update config.
  config_ = std::move(args.config);
  // Deactivate the children not in the new config.
  for (const auto& p : children_) {
    const std::string& name = p.first;
    ClusterChild* child = p.second.get();
    if (config_->cluster_map().find(name) == config_->cluster_map().end()) {
      child->DeactivateLocked();
    }
  }
  // Add or update the children in the new config.
  for (const auto& p : config_->cluster_map()) {
    const std::string& name = p.first;
    const RefCountedPtr<LoadBalancingPolicy::Config>& config = p.second;
    auto& child = children_[name];
    if (child == nullptr) {
      child = MakeOrphanable<ClusterChild>(Ref(DEBUG_LOCATION, "ClusterChild"),
                                           name);
    }
    child->UpdateLocked(config, args.addresses, args.args);
  }
  update_in_progress_ = false;
  UpdateStateLocked();
}

void XdsClusterManagerLb::UpdateStateLocked() {
  // If we're in the process of propagating an update from our parent to
  // our children, ignore any updates that come from the children.  We
  // will instead return a new picker once the update has been seen by
  // all children.  This avoids unnecessary picker churn while an update
  // is being propagated to our children.
  if (update_in_progress_) return;
  // Also count the number of children in each state, to determine the
  // overall state.
  size_t num_ready = 0;
  size_t num_connecting = 0;
  size_t num_idle = 0;
  size_t num_transient_failures = 0;
  for (const auto& p : children_) {
    const auto& child_name = p.first;
    const ClusterChild* child = p.second.get();
    // Skip the children that are not in the latest update.
    if (config_->cluster_map().find(child_name) ==
        config_->cluster_map().end()) {
      continue;
    }
    switch (child->connectivity_state()) {
      case GRPC_CHANNEL_READY: {
        ++num_ready;
        break;
      }
      case GRPC_CHANNEL_CONNECTING: {
        ++num_connecting;
        break;
      }
      case GRPC_CHANNEL_IDLE: {
        ++num_idle;
        break;
      }
      case GRPC_CHANNEL_TRANSIENT_FAILURE: {
        ++num_transient_failures;
        break;
      }
      default:
        GPR_UNREACHABLE_CODE(return );
    }
  }
  // Determine aggregated connectivity state.
  grpc_connectivity_state connectivity_state;
  if (num_ready > 0) {
    connectivity_state = GRPC_CHANNEL_READY;
  } else if (num_connecting > 0) {
    connectivity_state = GRPC_CHANNEL_CONNECTING;
  } else if (num_idle > 0) {
    connectivity_state = GRPC_CHANNEL_IDLE;
  } else {
    connectivity_state = GRPC_CHANNEL_TRANSIENT_FAILURE;
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_cluster_manager_lb_trace)) {
    gpr_log(GPR_INFO, "[xds_cluster_manager_lb %p] connectivity changed to %s",
            this, ConnectivityStateName(connectivity_state));
  }
  ClusterPicker::ClusterMap cluster_map;
  for (const auto& p : config_->cluster_map()) {
    const std::string& cluster_name = p.first;
    RefCountedPtr<ChildPickerWrapper>& child_picker = cluster_map[cluster_name];
    child_picker = children_[cluster_name]->picker_wrapper();
    if (child_picker == nullptr) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_cluster_manager_lb_trace)) {
        gpr_log(GPR_INFO,
                "[xds_cluster_manager_lb %p] child %s has not yet returned a "
                "picker; creating a QueuePicker.",
                this, cluster_name.c_str());
      }
      child_picker = MakeRefCounted<ChildPickerWrapper>(
          cluster_name,
          absl::make_unique<QueuePicker>(Ref(DEBUG_LOCATION, "QueuePicker")));
    }
  }
  std::unique_ptr<SubchannelPicker> picker =
      absl::make_unique<ClusterPicker>(std::move(cluster_map));
  absl::Status status;
  if (connectivity_state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
    status = absl::Status(absl::StatusCode::kUnavailable,
                          "TRANSIENT_FAILURE from XdsClusterManagerLb");
  }
  channel_control_helper()->UpdateState(connectivity_state, status,
                                        std::move(picker));
}

//
// XdsClusterManagerLb::ClusterChild
//

XdsClusterManagerLb::ClusterChild::ClusterChild(
    RefCountedPtr<XdsClusterManagerLb> xds_cluster_manager_policy,
    const std::string& name)
    : xds_cluster_manager_policy_(std::move(xds_cluster_manager_policy)),
      name_(name) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_cluster_manager_lb_trace)) {
    gpr_log(GPR_INFO,
            "[xds_cluster_manager_lb %p] created ClusterChild %p for %s",
            xds_cluster_manager_policy_.get(), this, name_.c_str());
  }
  GRPC_CLOSURE_INIT(&on_delayed_removal_timer_, OnDelayedRemovalTimer, this,
                    grpc_schedule_on_exec_ctx);
}

XdsClusterManagerLb::ClusterChild::~ClusterChild() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_cluster_manager_lb_trace)) {
    gpr_log(GPR_INFO,
            "[xds_cluster_manager_lb %p] ClusterChild %p: destroying "
            "child",
            xds_cluster_manager_policy_.get(), this);
  }
  xds_cluster_manager_policy_.reset(DEBUG_LOCATION, "ClusterChild");
}

void XdsClusterManagerLb::ClusterChild::Orphan() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_cluster_manager_lb_trace)) {
    gpr_log(GPR_INFO,
            "[xds_cluster_manager_lb %p] ClusterChild %p %s: "
            "shutting down child",
            xds_cluster_manager_policy_.get(), this, name_.c_str());
  }
  // Remove the child policy's interested_parties pollset_set from the
  // xDS policy.
  grpc_pollset_set_del_pollset_set(
      child_policy_->interested_parties(),
      xds_cluster_manager_policy_->interested_parties());
  child_policy_.reset();
  // Drop our ref to the child's picker, in case it's holding a ref to
  // the child.
  picker_wrapper_.reset();
  if (delayed_removal_timer_callback_pending_) {
    grpc_timer_cancel(&delayed_removal_timer_);
  }
  shutdown_ = true;
  Unref();
}

OrphanablePtr<LoadBalancingPolicy>
XdsClusterManagerLb::ClusterChild::CreateChildPolicyLocked(
    const ChannelArgs& args) {
  LoadBalancingPolicy::Args lb_policy_args;
  lb_policy_args.work_serializer =
      xds_cluster_manager_policy_->work_serializer();
  lb_policy_args.args = args;
  lb_policy_args.channel_control_helper =
      absl::make_unique<Helper>(this->Ref(DEBUG_LOCATION, "Helper"));
  OrphanablePtr<LoadBalancingPolicy> lb_policy =
      MakeOrphanable<ChildPolicyHandler>(std::move(lb_policy_args),
                                         &grpc_xds_cluster_manager_lb_trace);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_cluster_manager_lb_trace)) {
    gpr_log(GPR_INFO,
            "[xds_cluster_manager_lb %p] ClusterChild %p %s: Created "
            "new child "
            "policy handler %p",
            xds_cluster_manager_policy_.get(), this, name_.c_str(),
            lb_policy.get());
  }
  // Add the xDS's interested_parties pollset_set to that of the newly created
  // child policy. This will make the child policy progress upon activity on
  // xDS LB, which in turn is tied to the application's call.
  grpc_pollset_set_add_pollset_set(
      lb_policy->interested_parties(),
      xds_cluster_manager_policy_->interested_parties());
  return lb_policy;
}

void XdsClusterManagerLb::ClusterChild::UpdateLocked(
    RefCountedPtr<LoadBalancingPolicy::Config> config,
    const absl::StatusOr<ServerAddressList>& addresses,
    const ChannelArgs& args) {
  if (xds_cluster_manager_policy_->shutting_down_) return;
  // Update child weight.
  // Reactivate if needed.
  if (delayed_removal_timer_callback_pending_) {
    delayed_removal_timer_callback_pending_ = false;
    grpc_timer_cancel(&delayed_removal_timer_);
  }
  // Create child policy if needed.
  if (child_policy_ == nullptr) {
    child_policy_ = CreateChildPolicyLocked(args);
  }
  // Construct update args.
  UpdateArgs update_args;
  update_args.config = std::move(config);
  update_args.addresses = addresses;
  update_args.args = args;
  // Update the policy.
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_cluster_manager_lb_trace)) {
    gpr_log(GPR_INFO,
            "[xds_cluster_manager_lb %p] ClusterChild %p %s: "
            "Updating child "
            "policy handler %p",
            xds_cluster_manager_policy_.get(), this, name_.c_str(),
            child_policy_.get());
  }
  child_policy_->UpdateLocked(std::move(update_args));
}

void XdsClusterManagerLb::ClusterChild::ExitIdleLocked() {
  child_policy_->ExitIdleLocked();
}

void XdsClusterManagerLb::ClusterChild::ResetBackoffLocked() {
  child_policy_->ResetBackoffLocked();
}

void XdsClusterManagerLb::ClusterChild::DeactivateLocked() {
  // If already deactivated, don't do that again.
  if (delayed_removal_timer_callback_pending_) return;
  // Set the child weight to 0 so that future picker won't contain this child.
  // Start a timer to delete the child.
  Ref(DEBUG_LOCATION, "ClusterChild+timer").release();
  grpc_timer_init(&delayed_removal_timer_,
                  ExecCtx::Get()->Now() +
                      Duration::Milliseconds(
                          GRPC_XDS_CLUSTER_MANAGER_CHILD_RETENTION_INTERVAL_MS),
                  &on_delayed_removal_timer_);
  delayed_removal_timer_callback_pending_ = true;
}

void XdsClusterManagerLb::ClusterChild::OnDelayedRemovalTimer(
    void* arg, grpc_error_handle error) {
  ClusterChild* self = static_cast<ClusterChild*>(arg);
  (void)GRPC_ERROR_REF(error);  // Ref owned by the lambda
  self->xds_cluster_manager_policy_->work_serializer()->Run(
      [self, error]() { self->OnDelayedRemovalTimerLocked(error); },
      DEBUG_LOCATION);
}

void XdsClusterManagerLb::ClusterChild::OnDelayedRemovalTimerLocked(
    grpc_error_handle error) {
  delayed_removal_timer_callback_pending_ = false;
  if (GRPC_ERROR_IS_NONE(error) && !shutdown_) {
    xds_cluster_manager_policy_->children_.erase(name_);
  }
  Unref(DEBUG_LOCATION, "ClusterChild+timer");
  GRPC_ERROR_UNREF(error);
}

//
// XdsClusterManagerLb::ClusterChild::Helper
//

RefCountedPtr<SubchannelInterface>
XdsClusterManagerLb::ClusterChild::Helper::CreateSubchannel(
    ServerAddress address, const ChannelArgs& args) {
  if (xds_cluster_manager_child_->xds_cluster_manager_policy_->shutting_down_) {
    return nullptr;
  }
  return xds_cluster_manager_child_->xds_cluster_manager_policy_
      ->channel_control_helper()
      ->CreateSubchannel(std::move(address), args);
}

void XdsClusterManagerLb::ClusterChild::Helper::UpdateState(
    grpc_connectivity_state state, const absl::Status& status,
    std::unique_ptr<SubchannelPicker> picker) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_cluster_manager_lb_trace)) {
    gpr_log(
        GPR_INFO,
        "[xds_cluster_manager_lb %p] child %s: received update: state=%s (%s) "
        "picker=%p",
        xds_cluster_manager_child_->xds_cluster_manager_policy_.get(),
        xds_cluster_manager_child_->name_.c_str(), ConnectivityStateName(state),
        status.ToString().c_str(), picker.get());
  }
  if (xds_cluster_manager_child_->xds_cluster_manager_policy_->shutting_down_) {
    return;
  }
  // Cache the picker in the ClusterChild.
  xds_cluster_manager_child_->picker_wrapper_ =
      MakeRefCounted<ChildPickerWrapper>(xds_cluster_manager_child_->name_,
                                         std::move(picker));
  // Decide what state to report for aggregation purposes.
  // If the last recorded state was TRANSIENT_FAILURE and the new state
  // is something other than READY, don't change the state.
  if (xds_cluster_manager_child_->connectivity_state_ !=
          GRPC_CHANNEL_TRANSIENT_FAILURE ||
      state == GRPC_CHANNEL_READY) {
    xds_cluster_manager_child_->connectivity_state_ = state;
  }
  // Notify the LB policy.
  xds_cluster_manager_child_->xds_cluster_manager_policy_->UpdateStateLocked();
}

void XdsClusterManagerLb::ClusterChild::Helper::RequestReresolution() {
  if (xds_cluster_manager_child_->xds_cluster_manager_policy_->shutting_down_) {
    return;
  }
  xds_cluster_manager_child_->xds_cluster_manager_policy_
      ->channel_control_helper()
      ->RequestReresolution();
}

absl::string_view XdsClusterManagerLb::ClusterChild::Helper::GetAuthority() {
  return xds_cluster_manager_child_->xds_cluster_manager_policy_
      ->channel_control_helper()
      ->GetAuthority();
}

void XdsClusterManagerLb::ClusterChild::Helper::AddTraceEvent(
    TraceSeverity severity, absl::string_view message) {
  if (xds_cluster_manager_child_->xds_cluster_manager_policy_->shutting_down_) {
    return;
  }
  xds_cluster_manager_child_->xds_cluster_manager_policy_
      ->channel_control_helper()
      ->AddTraceEvent(severity, message);
}

//
// factory
//

class XdsClusterManagerLbFactory : public LoadBalancingPolicyFactory {
 public:
  OrphanablePtr<LoadBalancingPolicy> CreateLoadBalancingPolicy(
      LoadBalancingPolicy::Args args) const override {
    return MakeOrphanable<XdsClusterManagerLb>(std::move(args));
  }

  absl::string_view name() const override { return kXdsClusterManager; }

  absl::StatusOr<RefCountedPtr<LoadBalancingPolicy::Config>>
  ParseLoadBalancingConfig(const Json& json) const override {
    if (json.type() == Json::Type::JSON_NULL) {
      // xds_cluster_manager was mentioned as a policy in the deprecated
      // loadBalancingPolicy field or in the client API.
      return absl::InvalidArgumentError(
          "field:loadBalancingPolicy error:xds_cluster_manager policy requires "
          "configuration.  Please use loadBalancingConfig field of service "
          "config instead.");
    }
    std::vector<std::string> errors;
    XdsClusterManagerLbConfig::ClusterMap cluster_map;
    std::set<std::string /*cluster_name*/> clusters_to_be_used;
    auto it = json.object_value().find("children");
    if (it == json.object_value().end()) {
      errors.emplace_back("field:children error:required field not present");
    } else if (it->second.type() != Json::Type::OBJECT) {
      errors.emplace_back("field:children error:type should be object");
    } else {
      for (const auto& p : it->second.object_value()) {
        const std::string& child_name = p.first;
        if (child_name.empty()) {
          errors.emplace_back("field:children error: name cannot be empty");
          continue;
        }
        auto config = ParseChildConfig(p.second);
        if (!config.ok()) {
          errors.emplace_back(
              absl::StrCat("field:children name:", child_name,
                           " error:", config.status().message()));
        } else {
          cluster_map[child_name] = std::move(*config);
          clusters_to_be_used.insert(child_name);
        }
      }
    }
    if (cluster_map.empty()) {
      errors.emplace_back("no valid children configured");
    }
    if (!errors.empty()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "errors parsing xds_cluster_manager_experimental LB policy config: [",
          absl::StrJoin(errors, "; "), "]"));
    }
    return MakeRefCounted<XdsClusterManagerLbConfig>(std::move(cluster_map));
  }

 private:
  static absl::StatusOr<RefCountedPtr<LoadBalancingPolicy::Config>>
  ParseChildConfig(const Json& json) {
    if (json.type() != Json::Type::OBJECT) {
      return absl::InvalidArgumentError("value should be of type object");
    }
    RefCountedPtr<LoadBalancingPolicy::Config> child_config;
    std::vector<std::string> errors;
    auto it = json.object_value().find("childPolicy");
    if (it == json.object_value().end()) {
      errors.emplace_back("did not find childPolicy");
    } else {
      auto config = CoreConfiguration::Get()
                        .lb_policy_registry()
                        .ParseLoadBalancingConfig(it->second);
      if (!config.ok()) {
        errors.emplace_back(absl::StrCat("field:childPolicy error:",
                                         config.status().message()));
      } else {
        child_config = std::move(*config);
      }
    }
    if (!errors.empty()) {
      return absl::InvalidArgumentError(absl::StrJoin(errors, "; "));
    }
    return child_config;
  }
};

}  // namespace

void RegisterXdsClusterManagerLbPolicy(CoreConfiguration::Builder* builder) {
  builder->lb_policy_registry()->RegisterLoadBalancingPolicyFactory(
      absl::make_unique<XdsClusterManagerLbFactory>());
}

}  // namespace grpc_core

//
// Copyright 2019 gRPC authors.
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

#include "src/core/ext/xds/xds_bootstrap_grpc.h"

#include <stdlib.h>

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"

#include "src/core/lib/config/core_configuration.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/json/json.h"
#include "src/core/lib/json/json_util.h"
#include "src/core/lib/security/certificate_provider/certificate_provider_factory.h"
#include "src/core/lib/security/certificate_provider/certificate_provider_registry.h"
#include "src/core/lib/security/credentials/channel_creds_registry.h"

namespace grpc_core {

namespace {

grpc_error_handle ParseChannelCreds(const Json::Object& json, size_t idx,
                                    XdsBootstrap::XdsServer* server) {
  std::vector<grpc_error_handle> error_list;
  std::string type;
  ParseJsonObjectField(json, "type", &type, &error_list);
  const Json::Object* config_ptr = nullptr;
  ParseJsonObjectField(json, "config", &config_ptr, &error_list,
                       /*required=*/false);
  // Select the first channel creds type that we support.
  if (server->channel_creds_type.empty() &&
      CoreConfiguration::Get().channel_creds_registry().IsSupported(type)) {
    Json config;
    if (config_ptr != nullptr) config = *config_ptr;
    if (!CoreConfiguration::Get().channel_creds_registry().IsValidConfig(
            type, config)) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_CPP_STRING(absl::StrCat(
          "invalid config for channel creds type \"", type, "\"")));
    }
    server->channel_creds_type = std::move(type);
    server->channel_creds_config = std::move(config);
  }
  return GRPC_ERROR_CREATE_FROM_VECTOR_AND_CPP_STRING(
      absl::StrCat("errors parsing index ", idx), &error_list);
}

grpc_error_handle ParseChannelCredsArray(const Json::Array& json,
                                         XdsBootstrap::XdsServer* server) {
  std::vector<grpc_error_handle> error_list;
  for (size_t i = 0; i < json.size(); ++i) {
    const Json& child = json.at(i);
    if (child.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_CPP_STRING(
          absl::StrCat("array element ", i, " is not an object")));
    } else {
      grpc_error_handle parse_error =
          ParseChannelCreds(child.object_value(), i, server);
      if (!GRPC_ERROR_IS_NONE(parse_error)) error_list.push_back(parse_error);
    }
  }
  if (server->channel_creds_type.empty()) {
    error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "no known creds type found in \"channel_creds\""));
  }
  return GRPC_ERROR_CREATE_FROM_VECTOR("errors parsing \"channel_creds\" array",
                                       &error_list);
}

}  // namespace

//
// GrpcXdsBootstrap
//

std::unique_ptr<GrpcXdsBootstrap> GrpcXdsBootstrap::Create(
    absl::string_view json_string, grpc_error_handle* error) {
  auto json = Json::Parse(json_string);
  if (!json.ok()) {
    *error = GRPC_ERROR_CREATE_FROM_CPP_STRING(absl::StrCat(
        "Failed to parse bootstrap JSON string: ", json.status().ToString()));
    return nullptr;
  }
  return absl::make_unique<GrpcXdsBootstrap>(std::move(*json), error);
}

GrpcXdsBootstrap::GrpcXdsBootstrap(Json json, grpc_error_handle* error) {
  if (json.type() != Json::Type::OBJECT) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "malformed JSON in bootstrap file");
    return;
  }
  std::vector<grpc_error_handle> error_list;
  auto it = json.mutable_object()->find("xds_servers");
  if (it == json.mutable_object()->end()) {
    error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "\"xds_servers\" field not present"));
  } else if (it->second.type() != Json::Type::ARRAY) {
    error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "\"xds_servers\" field is not an array"));
  } else {
    grpc_error_handle parse_error = ParseXdsServerList(&it->second, &servers_);
    if (!GRPC_ERROR_IS_NONE(parse_error)) error_list.push_back(parse_error);
  }
  it = json.mutable_object()->find("node");
  if (it != json.mutable_object()->end()) {
    if (it->second.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "\"node\" field is not an object"));
    } else {
      grpc_error_handle parse_error = ParseNode(&it->second);
      if (!GRPC_ERROR_IS_NONE(parse_error)) error_list.push_back(parse_error);
    }
  }
  if (XdsFederationEnabled()) {
    it = json.mutable_object()->find("authorities");
    if (it != json.mutable_object()->end()) {
      if (it->second.type() != Json::Type::OBJECT) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "\"authorities\" field is not an object"));
      } else {
        grpc_error_handle parse_error = ParseAuthorities(&it->second);
        if (!GRPC_ERROR_IS_NONE(parse_error)) error_list.push_back(parse_error);
      }
    }
    it = json.mutable_object()->find(
        "client_default_listener_resource_name_template");
    if (it != json.mutable_object()->end()) {
      if (it->second.type() != Json::Type::STRING) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "\"client_default_listener_resource_name_template\" field is not a "
            "string"));
      } else {
        client_default_listener_resource_name_template_ =
            std::move(*it->second.mutable_string_value());
      }
    }
  }
  it = json.mutable_object()->find("server_listener_resource_name_template");
  if (it != json.mutable_object()->end()) {
    if (it->second.type() != Json::Type::STRING) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "\"server_listener_resource_name_template\" field is not a string"));
    } else {
      server_listener_resource_name_template_ =
          std::move(*it->second.mutable_string_value());
    }
  }
  it = json.mutable_object()->find("certificate_providers");
  if (it != json.mutable_object()->end()) {
    if (it->second.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "\"certificate_providers\" field is not an object"));
    } else {
      grpc_error_handle parse_error = ParseCertificateProviders(&it->second);
      if (!GRPC_ERROR_IS_NONE(parse_error)) error_list.push_back(parse_error);
    }
  }
  *error = GRPC_ERROR_CREATE_FROM_VECTOR("errors parsing xds bootstrap file",
                                         &error_list);
}

grpc_error_handle GrpcXdsBootstrap::ParseXdsServerList(
    Json* json, std::vector<XdsServer>* servers) {
  std::vector<grpc_error_handle> error_list;
  for (size_t i = 0; i < json->mutable_array()->size(); ++i) {
    Json& child = json->mutable_array()->at(i);
    if (child.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_CPP_STRING(
          absl::StrCat("array element ", i, " is not an object")));
    } else {
      grpc_error_handle parse_error;
      servers->emplace_back(XdsServerParse(child, &parse_error));
      if (!GRPC_ERROR_IS_NONE(parse_error)) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_CPP_STRING(
            absl::StrCat("errors parsing index ", i)));
        error_list.push_back(parse_error);
      }
    }
  }
  return GRPC_ERROR_CREATE_FROM_VECTOR("errors parsing \"xds_servers\" array",
                                       &error_list);
}

grpc_error_handle GrpcXdsBootstrap::ParseAuthorities(Json* json) {
  std::vector<grpc_error_handle> error_list;
  for (auto& p : *(json->mutable_object())) {
    if (p.second.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_CPP_STRING(
          "field:authorities element error: element is not a object"));
      continue;
    }
    grpc_error_handle parse_error = ParseAuthority(&p.second, p.first);
    if (!GRPC_ERROR_IS_NONE(parse_error)) error_list.push_back(parse_error);
  }
  return GRPC_ERROR_CREATE_FROM_VECTOR("errors parsing \"authorities\"",
                                       &error_list);
}

grpc_error_handle GrpcXdsBootstrap::ParseAuthority(Json* json,
                                                   const std::string& name) {
  std::vector<grpc_error_handle> error_list;
  Authority authority;
  auto it =
      json->mutable_object()->find("client_listener_resource_name_template");
  if (it != json->mutable_object()->end()) {
    if (it->second.type() != Json::Type::STRING) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "\"client_listener_resource_name_template\" field is not a string"));
    } else {
      std::string expected_prefix = absl::StrCat("xdstp://", name, "/");
      if (!absl::StartsWith(it->second.string_value(), expected_prefix)) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_CPP_STRING(
            absl::StrCat("\"client_listener_resource_name_template\" field "
                         "must begin with \"",
                         expected_prefix, "\"")));
      } else {
        authority.client_listener_resource_name_template =
            std::move(*it->second.mutable_string_value());
      }
    }
  }
  it = json->mutable_object()->find("xds_servers");
  if (it != json->mutable_object()->end()) {
    if (it->second.type() != Json::Type::ARRAY) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "\"xds_servers\" field is not an array"));
    } else {
      grpc_error_handle parse_error =
          ParseXdsServerList(&it->second, &authority.xds_servers);
      if (!GRPC_ERROR_IS_NONE(parse_error)) error_list.push_back(parse_error);
    }
  }
  if (error_list.empty()) {
    authorities_[name] = std::move(authority);
  }
  return GRPC_ERROR_CREATE_FROM_VECTOR_AND_CPP_STRING(
      absl::StrCat("errors parsing authority ", name), &error_list);
}

grpc_error_handle GrpcXdsBootstrap::ParseNode(Json* json) {
  std::vector<grpc_error_handle> error_list;
  node_ = absl::make_unique<Node>();
  auto it = json->mutable_object()->find("id");
  if (it != json->mutable_object()->end()) {
    if (it->second.type() != Json::Type::STRING) {
      error_list.push_back(
          GRPC_ERROR_CREATE_FROM_STATIC_STRING("\"id\" field is not a string"));
    } else {
      node_->id = std::move(*it->second.mutable_string_value());
    }
  }
  it = json->mutable_object()->find("cluster");
  if (it != json->mutable_object()->end()) {
    if (it->second.type() != Json::Type::STRING) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "\"cluster\" field is not a string"));
    } else {
      node_->cluster = std::move(*it->second.mutable_string_value());
    }
  }
  it = json->mutable_object()->find("locality");
  if (it != json->mutable_object()->end()) {
    if (it->second.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "\"locality\" field is not an object"));
    } else {
      grpc_error_handle parse_error = ParseLocality(&it->second);
      if (!GRPC_ERROR_IS_NONE(parse_error)) error_list.push_back(parse_error);
    }
  }
  it = json->mutable_object()->find("metadata");
  if (it != json->mutable_object()->end()) {
    if (it->second.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "\"metadata\" field is not an object"));
    } else {
      node_->metadata = std::move(it->second);
    }
  }
  return GRPC_ERROR_CREATE_FROM_VECTOR("errors parsing \"node\" object",
                                       &error_list);
}

grpc_error_handle GrpcXdsBootstrap::ParseLocality(Json* json) {
  std::vector<grpc_error_handle> error_list;
  auto it = json->mutable_object()->find("region");
  if (it != json->mutable_object()->end()) {
    if (it->second.type() != Json::Type::STRING) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "\"region\" field is not a string"));
    } else {
      node_->locality_region = std::move(*it->second.mutable_string_value());
    }
  }
  it = json->mutable_object()->find("zone");
  if (it != json->mutable_object()->end()) {
    if (it->second.type() != Json::Type::STRING) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "\"zone\" field is not a string"));
    } else {
      node_->locality_zone = std::move(*it->second.mutable_string_value());
    }
  }
  it = json->mutable_object()->find("sub_zone");
  if (it != json->mutable_object()->end()) {
    if (it->second.type() != Json::Type::STRING) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "\"sub_zone\" field is not a string"));
    } else {
      node_->locality_sub_zone = std::move(*it->second.mutable_string_value());
    }
  }
  return GRPC_ERROR_CREATE_FROM_VECTOR("errors parsing \"locality\" object",
                                       &error_list);
}

grpc_error_handle GrpcXdsBootstrap::ParseCertificateProviders(Json* json) {
  std::vector<grpc_error_handle> error_list;
  for (auto& certificate_provider : *(json->mutable_object())) {
    if (certificate_provider.second.type() != Json::Type::OBJECT) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_CPP_STRING(absl::StrCat(
          "element \"", certificate_provider.first, "\" is not an object")));
    } else {
      grpc_error_handle parse_error = ParseCertificateProvider(
          certificate_provider.first, &certificate_provider.second);
      if (!GRPC_ERROR_IS_NONE(parse_error)) error_list.push_back(parse_error);
    }
  }
  return GRPC_ERROR_CREATE_FROM_VECTOR(
      "errors parsing \"certificate_providers\" object", &error_list);
}

grpc_error_handle GrpcXdsBootstrap::ParseCertificateProvider(
    const std::string& instance_name, Json* certificate_provider_json) {
  std::vector<grpc_error_handle> error_list;
  auto it = certificate_provider_json->mutable_object()->find("plugin_name");
  if (it == certificate_provider_json->mutable_object()->end()) {
    error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "\"plugin_name\" field not present"));
  } else if (it->second.type() != Json::Type::STRING) {
    error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "\"plugin_name\" field is not a string"));
  } else {
    std::string plugin_name = std::move(*(it->second.mutable_string_value()));
    // Find config JSON.
    absl::optional<Json> config_json;
    it = certificate_provider_json->mutable_object()->find("config");
    if (it != certificate_provider_json->mutable_object()->end()) {
      if (it->second.type() != Json::Type::OBJECT) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "\"config\" field is not an object"));
      } else {
        config_json = it->second;
      }
    } else {
      // "config" is an optional field, so default to an empty JSON object.
      config_json = Json::Object();
    }
    // Try to instantiate the provider.
    CertificateProviderFactory* factory =
        CertificateProviderRegistry::LookupCertificateProviderFactory(
            plugin_name);
    if (factory == nullptr) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_CPP_STRING(
          absl::StrCat("Unrecognized plugin name: ", plugin_name)));
    } else if (config_json.has_value()) {
      grpc_error_handle parse_error = GRPC_ERROR_NONE;
      RefCountedPtr<CertificateProviderFactory::Config> config =
          factory->CreateCertificateProviderConfig(*config_json, &parse_error);
      if (!GRPC_ERROR_IS_NONE(parse_error)) {
        error_list.push_back(parse_error);
      } else {
        certificate_providers_.insert(
            {instance_name, {std::move(plugin_name), std::move(config)}});
      }
    }
  }
  return GRPC_ERROR_CREATE_FROM_VECTOR_AND_CPP_STRING(
      absl::StrCat("errors parsing element \"", instance_name, "\""),
      &error_list);
}

std::string GrpcXdsBootstrap::ToString() const {
  std::vector<std::string> parts;
  if (node_ != nullptr) {
    parts.push_back(absl::StrFormat(
        "node={\n"
        "  id=\"%s\",\n"
        "  cluster=\"%s\",\n"
        "  locality={\n"
        "    region=\"%s\",\n"
        "    zone=\"%s\",\n"
        "    sub_zone=\"%s\"\n"
        "  },\n"
        "  metadata=%s,\n"
        "},\n",
        node_->id, node_->cluster, node_->locality_region, node_->locality_zone,
        node_->locality_sub_zone, node_->metadata.Dump()));
  }
  parts.push_back(
      absl::StrFormat("servers=[\n"
                      "  {\n"
                      "    uri=\"%s\",\n"
                      "    creds_type=%s,\n",
                      server().server_uri, server().channel_creds_type));
  if (server().channel_creds_config.type() != Json::Type::JSON_NULL) {
    parts.push_back(absl::StrFormat("    creds_config=%s,",
                                    server().channel_creds_config.Dump()));
  }
  if (!server().server_features.empty()) {
    parts.push_back(absl::StrCat("    server_features=[",
                                 absl::StrJoin(server().server_features, ", "),
                                 "],\n"));
  }
  parts.push_back("  }\n],\n");
  if (!client_default_listener_resource_name_template_.empty()) {
    parts.push_back(absl::StrFormat(
        "client_default_listener_resource_name_template=\"%s\",\n",
        client_default_listener_resource_name_template_));
  }
  if (!server_listener_resource_name_template_.empty()) {
    parts.push_back(
        absl::StrFormat("server_listener_resource_name_template=\"%s\",\n",
                        server_listener_resource_name_template_));
  }
  parts.push_back("authorities={\n");
  for (const auto& entry : authorities_) {
    parts.push_back(absl::StrFormat("  %s={\n", entry.first));
    parts.push_back(
        absl::StrFormat("    client_listener_resource_name_template=\"%s\",\n",
                        entry.second.client_listener_resource_name_template));
    parts.push_back(
        absl::StrFormat("    servers=[\n"
                        "      {\n"
                        "        uri=\"%s\",\n"
                        "        creds_type=%s,\n",
                        entry.second.xds_servers[0].server_uri,
                        entry.second.xds_servers[0].channel_creds_type));
    parts.push_back("      },\n");
  }
  parts.push_back("}\n");
  parts.push_back("certificate_providers={\n");
  for (const auto& entry : certificate_providers_) {
    parts.push_back(
        absl::StrFormat("  %s={\n"
                        "    plugin_name=%s\n"
                        "    config=%s\n"
                        "  },\n",
                        entry.first, entry.second.plugin_name,
                        entry.second.config->ToString()));
  }
  parts.push_back("}");
  return absl::StrJoin(parts, "");
}

XdsBootstrap::XdsServer GrpcXdsBootstrap::XdsServerParse(
    const Json& json, grpc_error_handle* error) {
  std::vector<grpc_error_handle> error_list;
  XdsServer server;
  ParseJsonObjectField(json.object_value(), "server_uri", &server.server_uri,
                       &error_list);
  const Json::Array* creds_array = nullptr;
  ParseJsonObjectField(json.object_value(), "channel_creds", &creds_array,
                       &error_list);
  if (creds_array != nullptr) {
    grpc_error_handle parse_error =
        ParseChannelCredsArray(*creds_array, &server);
    if (!GRPC_ERROR_IS_NONE(parse_error)) error_list.push_back(parse_error);
  }
  const Json::Array* server_features_array = nullptr;
  ParseJsonObjectField(json.object_value(), "server_features",
                       &server_features_array, &error_list, /*required=*/false);
  if (server_features_array != nullptr) {
    for (const Json& feature_json : *server_features_array) {
      if (feature_json.type() == Json::Type::STRING &&
          (feature_json.string_value() ==
               XdsBootstrap::XdsServer::kServerFeatureXdsV3 ||
           feature_json.string_value() ==
               XdsBootstrap::XdsServer::kServerFeatureIgnoreResourceDeletion)) {
        server.server_features.insert(feature_json.string_value());
      }
    }
  }
  *error = GRPC_ERROR_CREATE_FROM_VECTOR_AND_CPP_STRING(
      "errors parsing xds server", &error_list);
  return server;
}

Json::Object GrpcXdsBootstrap::XdsServerToJson(const XdsServer& server) {
  Json::Object channel_creds_json{{"type", server.channel_creds_type}};
  if (server.channel_creds_config.type() != Json::Type::JSON_NULL) {
    channel_creds_json["config"] = server.channel_creds_config;
  }
  Json::Object json{
      {"server_uri", server.server_uri},
      {"channel_creds", Json::Array{std::move(channel_creds_json)}},
  };
  if (!server.server_features.empty()) {
    Json::Array server_features_json;
    for (auto& feature : server.server_features) {
      server_features_json.emplace_back(feature);
    }
    json["server_features"] = std::move(server_features_json);
  }
  return json;
}

}  // namespace grpc_core

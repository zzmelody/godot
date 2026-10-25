#pragma once

#include "core/io/resource.h"
#include "core/templates/hash_set.h"
#include "scene/resources/material.h"
#include "scene/resources/shader.h"
#include "scene/resources/shader_include.h"
#include "servers/rendering/rendering_server.h"

namespace CookerShaderFields {

// These values describe a Shader resource, not a material instance. Keep
// material overrides out of the record: ShaderMaterial parameter writes need
// not emit changed and must be read when applying a visual.
inline Dictionary Describe(const Ref<Shader>& shader) {
  Dictionary fields;
  fields["code"] = shader->get_code();
  bool range = false, shadow = false, lod = false, lod_scale = false;
  bool shareable = true;
  Dictionary texture_defaults;
  List<PropertyInfo> parameters;
  auto* server = RenderingServer::get_singleton();
  server->get_shader_parameter_list(shader->get_rid(), &parameters);
  for (const PropertyInfo& parameter : parameters) {
    const bool default_texture =
        shader->get_default_texture_parameter(parameter.name).is_valid();
    texture_defaults[parameter.name] = default_texture;
    const bool declared =
        !default_texture && (parameter.usage & (PROPERTY_USAGE_GROUP |
                                                PROPERTY_USAGE_SUBGROUP)) == 0;
    range |= declared && parameter.name == "visibility_range_end";
    shadow |= declared && parameter.name == "foliage_cast_shadows";
    lod |= declared && parameter.name == "foliage_lod_bias";
    lod_scale |= declared && parameter.name == "foliage_lod_scale";
    shareable &= parameter.type != Variant::ARRAY && !default_texture;
  }
  fields["range"] = range;
  fields["shadow"] = shadow;
  fields["lod"] = lod;
  fields["lod_scale"] = lod_scale;
  fields["texture_defaults"] = texture_defaults;
  Array includes;
  List<Object::Connection> inputs;
  shader->get_signals_connected_to_this(&inputs);
  for (const Object::Connection& input : inputs) {
    const Ref<ShaderInclude> include =
        Object::cast_to<ShaderInclude>(input.signal.get_object());
    if (include.is_valid()) {
      Dictionary dependency;
      dependency["resource"] = include;
      dependency["code"] = include->get_code();
      includes.push_back(dependency);
    }
  }
  fields["includes"] = includes;
  fields["shareable"] = shareable && !(parameters.is_empty() &&
                                       shader->get_code().contains("sampler"));
  fields["default_lod_bias"] = lod ? server->shader_get_parameter_default(
                                         shader->get_rid(), "foliage_lod_bias")
                                   : Variant();
  fields["default_lod_scale"] =
      lod_scale ? server->shader_get_parameter_default(shader->get_rid(),
                                                       "foliage_lod_scale")
                : Variant();
  return fields;
}

inline void Gather(const Variant& value, HashSet<ObjectID>& seen) {
  if (value.get_type() == Variant::OBJECT) {
    const Ref<Resource> resource = value;
    if (resource.is_null() || seen.has(resource->get_instance_id())) {
      return;
    }
    seen.insert(resource->get_instance_id());
    const Ref<ShaderMaterial> material = resource;
    if (material.is_valid()) {
      if (material->get_shader().is_valid()) {
        material->set_meta("veya_shader_fields",
                           Describe(material->get_shader()));
      }
      Gather(material->get_next_pass(), seen);
      return;
    }
    if (Ref<Shader>(resource).is_valid()) {
      return;
    }
    List<PropertyInfo> properties;
    resource->get_property_list(&properties);
    for (const PropertyInfo& property : properties) {
      if ((property.usage & PROPERTY_USAGE_STORAGE) &&
          !String(property.name).begins_with("metadata/")) {
        Gather(resource->get(property.name), seen);
      }
    }
  } else if (value.get_type() == Variant::ARRAY) {
    const Array values = value;
    for (int index = 0; index < values.size(); ++index) {
      Gather(values[index], seen);
    }
  } else if (value.get_type() == Variant::DICTIONARY) {
    const Dictionary values = value;
    const Array keys = values.keys();
    for (int index = 0; index < keys.size(); ++index) {
      Gather(values[keys[index]], seen);
    }
  }
}

inline void Prepare(const Ref<Resource>& resource) {
  // Bind through the material's Shader property. A separate Shader reference
  // in scene metadata can be serialized as a distinct built-in copy.
  HashSet<ObjectID> seen;
  Gather(resource, seen);
}

}  // namespace CookerShaderFields

#pragma once

#include "core/io/image.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/resources/portable_compressed_texture.h"

namespace CookerTextures {
inline thread_local int maximum_dimension = 1024;

class JobScope {
 public:
  explicit JobScope(const Dictionary& job) : previous_(maximum_dimension) {
    maximum_dimension = job.get("max_texture_dimension", 1024);
  }
  ~JobScope() {
    maximum_dimension = previous_;
  }

 private:
  int previous_;
};

inline Error PrepareImage(const Ref<Image>& image, int maximum, bool normal) {
  if (image.is_null() || image->is_empty()) {
    return ERR_INVALID_DATA;
  }
  if (image->is_compressed()) {
    const Error error = image->decompress();
    if (error != OK) {
      return error;
    }
  }
  const int size = MAX(image->get_width(), image->get_height());
  if (size > maximum) {
    const double scale = double(maximum) / size;
    image->resize(MAX(1, int(Math::round(image->get_width() * scale))),
                  MAX(1, int(Math::round(image->get_height() * scale))),
                  Image::INTERPOLATE_LANCZOS);
  }
  return image->generate_mipmaps(normal);
}

class Builder {
 public:
  Error Normalize(Variant& value, bool normal = false, int depth = 0) {
    bool replaced = false;
    return NormalizeValue(value, normal, depth, replaced);
  }

 private:
  Error NormalizeValue(Variant& value, bool normal, int depth, bool& replaced) {
    replaced = false;
    if (depth > 128) {
      return ERR_PARAMETER_RANGE_ERROR;
    }
    if (value.get_type() == Variant::ARRAY) {
      Array array = value;
      for (int index = 0; index < array.size(); ++index) {
        Variant child = array[index];
        bool child_replaced = false;
        const Error error =
            NormalizeValue(child, normal, depth + 1, child_replaced);
        if (error != OK) {
          return error;
        }
        if (child_replaced) {
          array[index] = child;
          replaced = true;
        }
      }
      return OK;
    }
    if (value.get_type() == Variant::DICTIONARY) {
      Dictionary dictionary = value;
      for (const Variant* key = dictionary.next(); key;
           key = dictionary.next(key)) {
        Variant child = dictionary[*key];
        bool child_replaced = false;
        const Error error =
            NormalizeValue(child, normal, depth + 1, child_replaced);
        if (error != OK) {
          return error;
        }
        if (child_replaced) {
          dictionary[*key] = child;
          replaced = true;
        }
      }
      return OK;
    }
    if (value.get_type() != Variant::OBJECT) {
      return OK;
    }
    Ref<Resource> resource = value;
    if (resource.is_null()) {
      return OK;
    }
    Ref<Texture2D> texture = resource;
    if (texture.is_valid()) {
      const auto id = texture->get_instance_id();
      if (const auto* prior = textures_.getptr(id)) {
        value = *prior;
        replaced = true;
        return OK;
      }
      Ref<Image> image = texture->get_image();
      if (image.is_null() || image->is_empty()) {
        return ERR_INVALID_DATA;
      }
      // Floating-point fields are data, not display textures. Their precision
      // and grid dimensions are part of the authored geometry contract.
      if (image->get_format() == Image::FORMAT_RF ||
          image->get_format() == Image::FORMAT_RGF ||
          image->get_format() == Image::FORMAT_RGBAF) {
        return OK;
      }
      const Ref<PortableCompressedTexture2D> portable = texture;
      if (portable.is_valid() &&
          portable->get_compression_mode() !=
              PortableCompressedTexture2D::COMPRESSION_MODE_LOSSLESS &&
          portable->get_compression_mode() !=
              PortableCompressedTexture2D::COMPRESSION_MODE_LOSSY &&
          image->has_mipmaps() &&
          MAX(image->get_width(), image->get_height()) <= maximum_dimension) {
        return OK;
      }
      image = image->duplicate();
      const Error error = PrepareImage(image, maximum_dimension, normal);
      if (error != OK) {
        return error;
      }
      Ref<PortableCompressedTexture2D> compressed;
      compressed.instantiate();
      compressed->set_keep_compressed_buffer(true);
      const bool hdr = image->get_format() == Image::FORMAT_RGBF ||
                       image->get_format() == Image::FORMAT_RGBH;
      compressed->create_from_image(
          image,
          hdr ? PortableCompressedTexture2D::COMPRESSION_MODE_BPTC
              : PortableCompressedTexture2D::COMPRESSION_MODE_BASIS_UNIVERSAL,
          normal);
      if (compressed->get_width() != image->get_width() ||
          compressed->get_height() != image->get_height()) {
        return ERR_CANT_CREATE;
      }
      textures_.insert(id, compressed);
      value = compressed;
      replaced = true;
      return OK;
    }
    if (visited_.has(resource->get_instance_id())) {
      return OK;
    }
    visited_.insert(resource->get_instance_id());
    List<PropertyInfo> properties;
    resource->get_property_list(&properties);
    for (const auto& property : properties) {
      if (!(property.usage & PROPERTY_USAGE_STORAGE) ||
          (property.type != Variant::OBJECT &&
           property.type != Variant::ARRAY &&
           property.type != Variant::DICTIONARY)) {
        continue;
      }
      Variant child = resource->get(property.name);
      const bool is_normal = String(property.name).contains("normal");
      bool child_replaced = false;
      const Error error =
          NormalizeValue(child, is_normal, depth + 1, child_replaced);
      if (error != OK) {
        return error;
      }
      // Material/Shader objects can change internally without changing their
      // binding. Do not rewrite immutable geometry dictionaries in that case.
      if (child_replaced) {
        resource->set(property.name, child);
      }
    }
    return OK;
  }

 private:
  HashSet<ObjectID> visited_;
  HashMap<ObjectID, Ref<Texture2D>> textures_;
};
}  // namespace CookerTextures
